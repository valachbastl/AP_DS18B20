#include "AP_DS18B20.h"
#include "onewire_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cmath>

const char *AP_DS18B20::TAG = "AP_DS18B20";

/* ------------------------------------------------------------------ */
/*  Constructor / destructor                                           */
/* ------------------------------------------------------------------ */

AP_DS18B20::AP_DS18B20(const Config &config)
{
    uint8_t fw        = config.filterWeight > 100 ? 100 : config.filterWeight;
    _filterNew        = (float)fw / 100.0f;
    _filterOld        = 1.0f - _filterNew;
    _errorIgnoreCount = config.errorIgnoreCount;
    _maxPowerResets   = config.maxPowerResets;
    _maxTempJump      = config.maxTempJump;
    _powerDelayUs     = (int64_t)config.powerDelayMs * 1000LL;
    _onPowerOn        = config.onPowerOn;
    _onPowerOff       = config.onPowerOff;
    _resolution       = config.resolution;
    _gpio_num         = config.gpio_num;

    // An out-of-range resolution would index past convDelayUs[] - clamp to 12 bit.
    if ((unsigned)_resolution > (unsigned)DS18B20_RESOLUTION_12B) {
        _resolution = DS18B20_RESOLUTION_12B;
    }
    static const int64_t convDelayUs[] = { 94000LL, 188000LL, 375000LL, 750000LL };
    _conversionDelayUs = convDelayUs[_resolution];
    // The 1-Wire bus and sensor enumeration are done only in begin() –
    // the constructor does not touch the hardware and does not abort the application.
}

esp_err_t AP_DS18B20::begin()
{
    esp_err_t ret = _initBus();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "begin: failed to create 1-Wire bus: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Found %d sensors", _sensorCount);

    // Sensors answered -> they are powered. Go straight to ON and skip the
    // IDLE -> ON power-up with its second enumeration. IDLE is kept only for
    // the "nothing found" case, where the sensors may just be unpowered.
    if (_sensorCount > 0 && _powerState == IDLE) {
        _powerState     = ON;
        _powerStateTime = esp_timer_get_time();
        if (_onPowerOn) _onPowerOn();
        ESP_LOGD(TAG, "Power ON (sensors found by begin)");
    }
    return ESP_OK;
}

esp_err_t AP_DS18B20::_initBus()
{
    // Keep the filter state of each physical sensor across a re-enumeration:
    // indices follow the bus order and shift when a sensor disappears, so the
    // states are matched back by ROM address below, not by position.
    SensorState oldStates[MAX_SENSORS];
    const uint8_t oldCount = _sensorCount;
    for (uint8_t i = 0; i < oldCount; i++) {
        oldStates[i] = _states[i];
    }

    // Release any old sensor handles and the bus (also called from _reinit after a power loss).
    for (uint8_t i = 0; i < _sensorCount; i++) {
        ds18b20_del_device(_sensors[i]);
    }
    _sensorCount = 0;

    if (_bus != nullptr) {
        onewire_bus_del(_bus);
        _bus = nullptr;
    }

    onewire_bus_config_t     busCfg = { .bus_gpio_num = _gpio_num };
    // max_rx_bytes is given by the DS18B20 protocol: the largest read is the 9 B scratchpad
    // (8 data + CRC), 10 = +1 B margin. Protocol constant, do not tune.
    onewire_bus_rmt_config_t rmtCfg = { .max_rx_bytes = 10 };
    esp_err_t ret = onewire_new_bus_rmt(&busCfg, &rmtCfg, &_bus);
    if (ret != ESP_OK) {
        _bus = nullptr;
        return ret;
    }

    onewire_device_iter_handle_t iter;
    ret = onewire_new_device_iter(_bus, &iter);
    if (ret != ESP_OK) {
        return ret;
    }

    onewire_device_t device;
    while (onewire_device_iter_get_next(iter, &device) == ESP_OK) {
        if (_sensorCount >= MAX_SENSORS) {
            ESP_LOGW(TAG, "Max. sensor count reached (%d), others ignored", MAX_SENSORS);
            break;
        }
        ds18b20_config_t dsCfg = {};
        if (ds18b20_new_device_from_enumeration(&device, &dsCfg, &_sensors[_sensorCount]) == ESP_OK) {
            ds18b20_set_resolution(_sensors[_sensorCount], _resolution);
            ESP_LOGI(TAG, "DS18B20[%d] address: %016llX", _sensorCount, device.address);

            SensorState state;  // new sensor: fresh state
            state.address = device.address;
            for (uint8_t j = 0; j < oldCount; j++) {
                if (oldStates[j].address == device.address) {
                    state = oldStates[j];  // known sensor: keep its filter state
                    break;
                }
            }
            _states[_sensorCount] = state;
            _sensorCount++;
        }
    }

    onewire_del_device_iter(iter);
    return ESP_OK;
}

void AP_DS18B20::_reinit()
{
    _pendingReinit = false;

    // Full re-initialization after a power loss – same init as begin().
    esp_err_t ret = _initBus();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reinit: failed to create 1-Wire bus: %s", esp_err_to_name(ret));
        _pendingReinit = true;  // retry next cycle
        return;
    }

    if (_sensorCount == 0) {
        ESP_LOGW(TAG, "Reinit: no sensor found, will retry");
        _pendingReinit = true;  // retry next cycle
    } else {
        ESP_LOGI(TAG, "Reinit: found %d sensors", _sensorCount);
    }
}

AP_DS18B20::~AP_DS18B20()
{
    for (uint8_t i = 0; i < _sensorCount; i++) {
        ds18b20_del_device(_sensors[i]);
    }
    if (_bus != nullptr) {
        onewire_bus_del(_bus);
    }
}

/* ------------------------------------------------------------------ */
/*  Conversion + filtered read                                         */
/* ------------------------------------------------------------------ */

esp_err_t AP_DS18B20::convertAll()
{
    // Timing: enforces the conversion delay as well as the delay after power restore (tick sets _lastConvertTimeUs=now)
    if ((esp_timer_get_time() - _lastConvertTimeUs) < _conversionDelayUs) {
        return ESP_OK;
    }

    // Reinit after power restore - runs only once the delay has elapsed
    if (_pendingReinit) {
        _reinit();
        if (_sensorCount == 0) {
            _lastConvertTimeUs = esp_timer_get_time();  // rate-limit repeated attempts
            return ESP_OK;
        }
    }

    if (_sensorCount == 0) return ESP_OK;

    esp_err_t ret = ds18b20_trigger_temperature_conversion_for_all(_bus);
    if (ret == ESP_OK) {
        _lastConvertTimeUs = esp_timer_get_time();
    } else {
        ESP_LOGW(TAG, "Conversion error: %s", esp_err_to_name(ret));
    }

    // Always set readPending - if the conversion failed, getTempFiltered handles the error
    for (uint8_t i = 0; i < _sensorCount; i++) {
        _states[i].readPending = true;
    }
    return ret;
}

esp_err_t AP_DS18B20::getTempFiltered(uint8_t index, float &temperature)
{
    temperature = NO_SENSOR_TEMP;
    if (index >= _sensorCount) return ESP_ERR_INVALID_ARG;

    SensorState &s = _states[index];

    // Conversion not started or not finished yet - return the last known value
    if (!s.readPending || (esp_timer_get_time() - _lastConvertTimeUs) < _conversionDelayUs) {
        temperature = s.hasValue ? s.ewaValue : NO_SENSOR_TEMP;
        return ESP_OK;
    }
    s.readPending = false;

    float raw = 0.0f;
    esp_err_t ret = ds18b20_get_temperature(_sensors[index], &raw);

    // Bad reading detection: communication error, value below -125,
    // power-on reset value 85°C (as in the official DallasTemperature library),
    // or too large a jump from the last valid value (bus noise)
    // Note: phantom 25°C check removed - caused false positives at room temperature ~25°C
    bool isError = (ret != ESP_OK)
                || (raw < -125.0f)
                || (raw == 85.0f && s.hasValue && s.ewaValue != 85.0f)
                || (s.hasValue && s.ewaValue > -125.0f && fabsf(raw - s.ewaValue) > _maxTempJump);

    if (isError) {
        if (s.errorCount < 255) s.errorCount++;
    } else {
        s.errorCount  = 0;
        s.powerResets = 0;  // per sensor - a healthy sensor must not reset the limit of a failing one
    }

    // EWA filter update:
    //   errorCount == 0                → normal update
    //   1 .. errorIgnoreCount          → hold the last good value (transient glitch)
    //   > errorIgnoreCount             → accept the bad value, trigger a power reset
    if (s.errorCount == 0 || s.errorCount > _errorIgnoreCount) {
        float val = (ret != ESP_OK) ? INVALID_TEMP : raw;
        if (!s.hasValue || s.ewaValue < -125.0f || val < -125.0f) {
            s.ewaValue = val;  // first reading or recovery after an error: direct write without EWA
        } else {
            s.ewaValue = (s.ewaValue * _filterOld) + (val * _filterNew);
        }
        s.hasValue = true;
    }

    // Schedule a power reset on every (errorIgnoreCount+1)-th consecutive error.
    // 16-bit so that errorIgnoreCount = 255 cannot wrap the divisor to zero.
    // The power line is shared by all sensors, the maxPowerResets limit is per sensor.
    const uint16_t threshold = (uint16_t)_errorIgnoreCount + 1;
    if (s.errorCount > 0
     && (s.errorCount % threshold) == 0
     && s.powerResets < _maxPowerResets
     && _powerState == ON) {
        _powerState     = OFF_REQ;
        _powerStateTime = esp_timer_get_time();
        s.powerResets++;
        ESP_LOGW(TAG, "Sensor %d: power-off scheduled #%d", index, s.powerResets);
    }

    temperature = s.hasValue ? s.ewaValue : INVALID_TEMP;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Power reset state machine                                          */
/* ------------------------------------------------------------------ */

void AP_DS18B20::tick()
{
    int64_t now     = esp_timer_get_time();
    bool    delayOk = (now - _powerStateTime) >= _powerDelayUs;

    switch (_powerState) {
        case IDLE:
        case OFF:
            if (delayOk) {
                _powerState        = ON;
                _powerStateTime    = now;
                _lastConvertTimeUs = now;   // give the DS18B20 time to initialize (at least one task cycle)
                _pendingReinit     = true;  // reinit bus + sensors only after the delay has elapsed
                for (uint8_t i = 0; i < _sensorCount; i++) {
                    _states[i].readPending = false;
                    _states[i].errorCount  = 0;
                }
                if (_onPowerOn) _onPowerOn();
                ESP_LOGD(TAG, "Power ON");
            }
            break;

        case OFF_REQ:
            if (delayOk) {
                _powerState     = OFF;
                _powerStateTime = now;
                if (_onPowerOff) _onPowerOff();
                ESP_LOGD(TAG, "Power OFF");
            }
            break;

        case ON:
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Helper methods                                                     */
/* ------------------------------------------------------------------ */

uint8_t AP_DS18B20::getSensorCount() const
{
    return _sensorCount;
}

uint64_t AP_DS18B20::getAddress(uint8_t index) const
{
    return (index < _sensorCount) ? _states[index].address : 0;
}

esp_err_t AP_DS18B20::getTempByAddress(uint64_t address, float &temperature)
{
    for (uint8_t i = 0; i < _sensorCount; i++) {
        if (_states[i].address == address) {
            return getTempFiltered(i, temperature);
        }
    }
    temperature = NO_SENSOR_TEMP;
    return ESP_ERR_NOT_FOUND;
}
