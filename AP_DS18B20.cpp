#include "AP_DS18B20.h"
#include "onewire_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cmath>

const char *AP_DS18B20::TAG = "AP_DS18B20";

/* ------------------------------------------------------------------ */
/*  Konstruktor / destruktor                                           */
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

    static const int64_t convDelayUs[] = { 94000LL, 188000LL, 375000LL, 750000LL };
    _conversionDelayUs = convDelayUs[_resolution];

    onewire_bus_config_t     busCfg = { .bus_gpio_num = _gpio_num };
    onewire_bus_rmt_config_t rmtCfg = { .max_rx_bytes = 10 };
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&busCfg, &rmtCfg, &_bus));

    onewire_device_iter_handle_t iter;
    ESP_ERROR_CHECK(onewire_new_device_iter(_bus, &iter));

    onewire_device_t device;
    while (onewire_device_iter_get_next(iter, &device) == ESP_OK) {
        if (_sensorCount >= MAX_SENSORS) {
            ESP_LOGW(TAG, "Dosazeno max. poctu senzoru (%d), dalsi ignorovany", MAX_SENSORS);
            break;
        }
        ds18b20_config_t dsCfg = {};
        if (ds18b20_new_device_from_enumeration(&device, &dsCfg, &_sensors[_sensorCount]) == ESP_OK) {
            ds18b20_set_resolution(_sensors[_sensorCount], _resolution);
            ESP_LOGI(TAG, "DS18B20[%d] adresa: %016llX", _sensorCount, device.address);
            _sensorCount++;
        }
    }

    onewire_del_device_iter(iter);
    ESP_LOGI(TAG, "Nalezeno %d senzoru", _sensorCount);
}

void AP_DS18B20::_reinit()
{
    _pendingReinit = false;

    // Zrus stare handles senzoru
    for (uint8_t i = 0; i < _sensorCount; i++) {
        ds18b20_del_device(_sensors[i]);
    }
    _sensorCount = 0;

    // Zrus a znovu vytvor RMT bus - cisti interni stav driveru po vypadku napajeni senzoru
    if (_bus != nullptr) {
        onewire_bus_del(_bus);
        _bus = nullptr;
    }

    onewire_bus_config_t     busCfg = { .bus_gpio_num = _gpio_num };
    onewire_bus_rmt_config_t rmtCfg = { .max_rx_bytes = 10 };
    if (onewire_new_bus_rmt(&busCfg, &rmtCfg, &_bus) != ESP_OK) {
        ESP_LOGE(TAG, "Reinit: selhalo vytvoreni bus");
        _pendingReinit = true;
        return;
    }

    // Znovu enumeruj senzory
    onewire_device_iter_handle_t iter;
    if (onewire_new_device_iter(_bus, &iter) != ESP_OK) {
        ESP_LOGE(TAG, "Reinit: selhalo vytvoreni iter");
        _pendingReinit = true;
        return;
    }

    onewire_device_t device;
    while (onewire_device_iter_get_next(iter, &device) == ESP_OK) {
        if (_sensorCount >= MAX_SENSORS) {
            ESP_LOGW(TAG, "Dosazeno max. poctu senzoru (%d), dalsi ignorovany", MAX_SENSORS);
            break;
        }
        ds18b20_config_t dsCfg = {};
        if (ds18b20_new_device_from_enumeration(&device, &dsCfg, &_sensors[_sensorCount]) == ESP_OK) {
            ds18b20_set_resolution(_sensors[_sensorCount], _resolution);
            ESP_LOGI(TAG, "Reinit DS18B20[%d]: %016llX", _sensorCount, device.address);
            _sensorCount++;
        }
    }

    onewire_del_device_iter(iter);

    if (_sensorCount == 0) {
        ESP_LOGW(TAG, "Reinit: zadny senzor nenalezen, zkusit znovu");
        _pendingReinit = true;  // zkusit znovu pristi cyklus
    } else {
        ESP_LOGI(TAG, "Reinit: nalezeno %d senzoru", _sensorCount);
    }
}

AP_DS18B20::~AP_DS18B20()
{
    for (uint8_t i = 0; i < _sensorCount; i++) {
        ds18b20_del_device(_sensors[i]);
    }
    onewire_bus_del(_bus);
}

/* ------------------------------------------------------------------ */
/*  Konverze + cteni s filtrem                                         */
/* ------------------------------------------------------------------ */

esp_err_t AP_DS18B20::convertAll()
{
    // Casovani: zajistuje konverzni prodlevu i prodlevu po obnove napajeni (tick nastavi _lastConvertTimeUs=now)
    if ((esp_timer_get_time() - _lastConvertTimeUs) < _conversionDelayUs) {
        return ESP_OK;
    }

    // Reinit po obnove napajeni - spusti se az po ubehnuti prodlevy
    if (_pendingReinit) {
        _reinit();
        if (_sensorCount == 0) {
            _lastConvertTimeUs = esp_timer_get_time();  // rate-limit opakovanych pokusu
            return ESP_OK;
        }
    }

    if (_sensorCount == 0) return ESP_OK;

    esp_err_t ret = ds18b20_trigger_temperature_conversion_for_all(_bus);
    if (ret == ESP_OK) {
        _lastConvertTimeUs = esp_timer_get_time();
    } else {
        ESP_LOGW(TAG, "Chyba konverze: %s", esp_err_to_name(ret));
    }

    // readPending nastavit vzdy - pri selhani konverze zpracuje chybu getTempFiltered
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

    // Konverze nebyla spustena nebo jeste nebyla dokoncena - vrat posledni znamou hodnotu
    if (!s.readPending || (esp_timer_get_time() - _lastConvertTimeUs) < _conversionDelayUs) {
        temperature = s.hasValue ? s.ewaValue : NO_SENSOR_TEMP;
        return ESP_OK;
    }
    s.readPending = false;

    float raw = 0.0f;
    esp_err_t ret = ds18b20_get_temperature(_sensors[index], &raw);

    // Detekce chybneho mereni: chyba komunikace, hodnota pod -125,
    // power-on reset hodnota 85°C (dle official DallasTemperature knihovny),
    // nebo prilis velky skok oproti posledni platne hodnote (ruseni na sbernici)
    // Pozn: phantom 25°C check odstranen - pri pokojove teplote ~25°C zpusoboval false positive
    bool isError = (ret != ESP_OK)
                || (raw < -125.0f)
                || (raw == 85.0f && s.hasValue && s.ewaValue != 85.0f)
                || (s.hasValue && s.ewaValue > -125.0f && fabsf(raw - s.ewaValue) > _maxTempJump);

    if (isError) {
        if (s.errorCount < 255) s.errorCount++;
    } else {
        s.errorCount     = 0;
        _powerResetCount = 0;
    }

    // Aktualizace EWA filtru:
    //   errorCount == 0                → normalni aktualizace
    //   1 .. errorIgnoreCount          → drz posledni dobrou hodnotu (prechodny glitch)
    //   > errorIgnoreCount             → akceptuj spatnou hodnotu, spust reset napajeni
    if (s.errorCount == 0 || s.errorCount > _errorIgnoreCount) {
        float val = (ret != ESP_OK) ? INVALID_TEMP : raw;
        if (!s.hasValue || s.ewaValue < -125.0f || val < -125.0f) {
            s.ewaValue = val;  // prvni mereni nebo obnova po chybe: primy zapis bez EWA
        } else {
            s.ewaValue = (s.ewaValue * _filterOld) + (val * _filterNew);
        }
        s.hasValue = true;
    }

    // Naplanovani resetu napajeni pri kazdem (errorIgnoreCount+1)-nasobku po sobe jdoucich chyb
    uint8_t threshold = _errorIgnoreCount + 1;
    if (s.errorCount > 0
     && (s.errorCount % threshold) == 0
     && _powerResetCount < _maxPowerResets
     && _powerState == ON) {
        _powerState     = OFF_REQ;
        _powerStateTime = esp_timer_get_time();
        _powerResetCount++;
        ESP_LOGW(TAG, "Senzor %d: naplanovano vypnuti napajeni #%d", index, _powerResetCount);
    }

    temperature = s.hasValue ? s.ewaValue : INVALID_TEMP;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  State machine resetu napajeni                                      */
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
                _lastConvertTimeUs = now;   // dat DS18B20 cas na inicializaci (min. jeden task cyklus)
                _pendingReinit     = true;  // reinit bus + senzory az po ubehnuti prodlevy
                for (uint8_t i = 0; i < _sensorCount; i++) {
                    _states[i].readPending = false;
                    _states[i].errorCount  = 0;
                }
                if (_onPowerOn) _onPowerOn();
                ESP_LOGD(TAG, "Napajeni ON");
            }
            break;

        case OFF_REQ:
            if (delayOk) {
                _powerState     = OFF;
                _powerStateTime = now;
                if (_onPowerOff) _onPowerOff();
                ESP_LOGD(TAG, "Napajeni OFF");
            }
            break;

        case ON:
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Pomocne metody                                                     */
/* ------------------------------------------------------------------ */

uint8_t AP_DS18B20::getSensorCount() const
{
    return _sensorCount;
}
