#pragma once

#include "onewire_bus.h"
#include "ds18b20.h"
#include "esp_err.h"
#include <functional>
#include <stdint.h>

/**
 * @brief DS18B20 1-Wire temperature driver with EWA filter, error detection and
 *        a power reset state machine.
 *
 * Ownership: one instance, one owning task. It is not internally locked - call
 * tick() / getTempFiltered() / convertAll() from the same task and fan the
 * values out to other tasks through a queue.
 */
class AP_DS18B20
{
public:
    static constexpr float NO_SENSOR_TEMP = -126.0f;  // no value: sensor not found / index out of range, or no reading yet
    static constexpr float INVALID_TEMP   = -127.0f;  // sensor found, but the read failed

    struct Config {
        int      gpio_num;
        uint16_t powerDelayMs      = 5000;                       // minimum delay between power switching [ms]
        uint8_t  filterWeight      = 80;                         // weight of the new value in the EWA filter, 0-100
        uint8_t  errorIgnoreCount  = 3;                          // consecutive errors before a bad value is accepted
        uint8_t  maxPowerResets    = 3;                          // max consecutive power resets caused by one sensor (reset by its good reading)
        float    maxTempJump       = 10.0f;                      // max allowed temperature jump between readings [°C], larger = glitch
        ds18b20_resolution_t resolution  = DS18B20_RESOLUTION_12B; // measurement resolution (12b=0.0625°C/750ms, 9b=0.5°C/94ms), invalid = 12b
        std::function<void()> onPowerOn  = nullptr;              // called when sensor power is switched on (from tick(), or from begin() if it found sensors)
        std::function<void()> onPowerOff = nullptr;              // called when sensor power is switched off (from tick())
    };

    /**
     * @brief Constructor - only stores the configuration, does not touch the hardware
     * @param config Configuration
     */
    explicit AP_DS18B20(const Config &config);

    ~AP_DS18B20();

    AP_DS18B20(const AP_DS18B20 &) = delete;
    AP_DS18B20 &operator=(const AP_DS18B20 &) = delete;

    /**
     * @brief Initializes the 1-Wire bus and auto-detects DS18B20 sensors.
     *        Must be called before the first read / getSensorCount(). Returns
     *        esp_err_t instead of aborting on error.
     *        If sensors are found, they are evidently powered: the power state
     *        machine goes straight to ON and onPowerOn is called from here
     *        (no second enumeration). If none are found, the first tick() after
     *        powerDelayMs switches power on and re-enumerates.
     * @return ESP_OK on success (even if no sensor is found), otherwise the error
     *         from creating the 1-Wire bus
     */
    esp_err_t begin();

    /**
     * @brief Starts a temperature conversion on all sensors.
     *        Ignored if the previous conversion has not finished yet.
     * @return ESP_OK on success or when skipped, error code on bus failure
     */
    esp_err_t convertAll();

    /**
     * @brief Reads and filters the temperature of the sensor at the given index.
     *        If no conversion was started or it has not finished yet, returns the last known value.
     *        Applies the EWA filter, error detection and schedules a power reset when needed.
     * @param index Sensor index (0 to getSensorCount()-1)
     * @param temperature Output filtered temperature [°C]; NO_SENSOR_TEMP if the index is out
     *                    of range or there is no reading yet (e.g. right after begin() or a
     *                    power reset); INVALID_TEMP if the read failed and there is no good
     *                    value to hold (errors past errorIgnoreCount, or failing from the start)
     * @return ESP_OK on success, ESP_ERR_INVALID_ARG if the index is out of range
     */
    esp_err_t getTempFiltered(uint8_t index, float &temperature);

    /**
     * @brief Same as getTempFiltered(), but selects the sensor by its ROM address.
     *        Unlike the index (bus enumeration order), the address identifies a
     *        physical sensor - it stays correct when indices shift after a sensor
     *        disappears and the bus is re-initialized.
     * @param address ROM address (see getAddress())
     * @param temperature Output filtered temperature [°C], see getTempFiltered()
     * @return ESP_OK on success, ESP_ERR_NOT_FOUND (temperature = NO_SENSOR_TEMP) if no
     *         sensor with this address is currently on the bus
     */
    esp_err_t getTempByAddress(uint64_t address, float &temperature);

    /**
     * @brief Returns the 64-bit ROM address of the sensor at the given index -
     *        e.g. to list the sensors and bind them to roles in the application.
     * @return ROM address, or 0 if the index is out of range (0 is never a valid
     *         DS18B20 address - family code 0x28)
     */
    uint64_t getAddress(uint8_t index) const;

    /**
     * @brief Runs the power reset state machine - call periodically (~100 ms)
     *        Calls the onPowerOn / onPowerOff callbacks at the right time
     */
    void tick();

    /**
     * @brief Returns the number of sensors found
     */
    uint8_t getSensorCount() const;

private:
    // Upper limit of sensors on the bus – sets the size of the static arrays _sensors/_states
    // (no heap → deterministic, no fragmentation). 8 is a practical ceiling for a single
    // 1-Wire bus; raise the constant for more.
    static constexpr uint8_t MAX_SENSORS = 8;
    static const char *TAG;

    struct SensorState {
        uint64_t address     = 0;      // ROM address - keys the state across re-enumerations
        float    ewaValue    = 0.0f;
        uint8_t  errorCount  = 0;
        uint8_t  powerResets = 0;      // consecutive power resets caused by this sensor
        bool     hasValue    = false;
        bool     readPending = false;  // true after convertAll(), false once the result is read
    };

    enum PowerState : uint8_t {
        IDLE    = 0,  // initial state
        ON      = 1,  // power on, normal operation
        OFF_REQ = 2,  // power-off requested (sensor error)
        OFF     = 3,  // power off, waiting to switch on
    };

    onewire_bus_handle_t    _bus = nullptr;
    ds18b20_device_handle_t _sensors[MAX_SENSORS];
    SensorState             _states[MAX_SENSORS];
    uint8_t                 _sensorCount   = 0;

    int                  _gpio_num;
    float                _filterNew;
    float                _filterOld;
    uint8_t              _errorIgnoreCount;
    uint8_t              _maxPowerResets;
    float                _maxTempJump;
    ds18b20_resolution_t _resolution;
    int64_t              _lastConvertTimeUs = 0;
    int64_t              _conversionDelayUs;
    bool                 _pendingReinit     = false;

    PowerState _powerState     = IDLE;
    int64_t    _powerStateTime = 0;
    int64_t    _powerDelayUs;

    std::function<void()> _onPowerOn;
    std::function<void()> _onPowerOff;

    esp_err_t _initBus();   // creates the RMT bus + enumerates sensors (shared by begin() and _reinit())
    void _reinit();
};
