#pragma once

#include "onewire_bus.h"
#include "ds18b20.h"
#include "esp_err.h"
#include <functional>
#include <stdint.h>

class AP_DS18B20
{
public:
    static constexpr float NO_SENSOR_TEMP = -126.0f;  // senzor nenalezen pri enumeraci nebo index mimo rozsah
    static constexpr float INVALID_TEMP   = -127.0f;  // senzor nalezen, ale cteni selhalo

    struct Config {
        int      gpio_num;
        uint16_t powerDelayMs      = 5000;                       // minimalni prodleva mezi prepnutim napajeni [ms]
        uint8_t  filterWeight      = 80;                         // vaha nove hodnoty v EWA filtru, 0-100
        uint8_t  errorIgnoreCount  = 3;                          // pocet po sobe jdoucich chyb pred akceptaci spatne hodnoty
        uint8_t  maxPowerResets    = 3;                          // max pocet cyklu resetu napajeni
        float    maxTempJump       = 10.0f;                      // max povoleny skok teploty mezi merenima [°C], vetsi = glitch
        ds18b20_resolution_t resolution  = DS18B20_RESOLUTION_12B; // rozliseni mereni (12b=0.0625°C/750ms, 9b=0.5°C/94ms)
        std::function<void()> onPowerOn  = nullptr;              // zavolano pri zapnuti napajeni senzoru
        std::function<void()> onPowerOff = nullptr;              // zavolano pri vypnuti napajeni senzoru
    };

    /**
     * @brief Konstruktor - inicializuje 1-Wire sbernici a autodetekcí nalezne DS18B20 senzory
     * @param config Konfigurace
     */
    AP_DS18B20(const Config &config);

    ~AP_DS18B20();

    AP_DS18B20(const AP_DS18B20 &) = delete;
    AP_DS18B20 &operator=(const AP_DS18B20 &) = delete;

    /**
     * @brief Spusti konverzi teploty na vsech senzorech.
     *        Pokud jeste nebyla dokoncena predchozi konverze, volani se ignoruje.
     * @return ESP_OK pri uspechu nebo preskoceni, chybovy kod pri selhani sbernice
     */
    esp_err_t convertAll();

    /**
     * @brief Precte a filtruje teplotu senzoru na danem indexu.
     *        Pokud konverze jeste nebyla spustena nebo nedokoncena, vrati posledni znamou hodnotu.
     *        Aplikuje EWA filtr, detekci chyb a v pripade potreby naplanova reset napajeni.
     * @param index Index senzoru (0 az getSensorCount()-1)
     * @param temperature Vystupni filtrovana teplota [°C], INVALID_TEMP pokud jeste neni zadna hodnota
     * @return ESP_OK pri uspechu, ESP_ERR_INVALID_ARG pokud index mimo rozsah
     */
    esp_err_t getTempFiltered(uint8_t index, float &temperature);

    /**
     * @brief Zpracuje state machine resetu napajeni - volat periodicky (~100 ms)
     *        Vola onPowerOn / onPowerOff callbacky ve spravny cas
     */
    void tick();

    /**
     * @brief Vrati pocet nalezenych senzoru
     */
    uint8_t getSensorCount() const;

private:
    static constexpr uint8_t MAX_SENSORS = 8;
    static const char *TAG;

    struct SensorState {
        float   ewaValue    = 0.0f;
        uint8_t errorCount  = 0;
        bool    hasValue    = false;
        bool    readPending = false;  // true po convertAll(), false po precteni vysledku
    };

    enum PowerState : uint8_t {
        IDLE    = 0,  // pocatecni stav
        ON      = 1,  // napajeni zapnuto, normalni provoz
        OFF_REQ = 2,  // zadost o vypnuti napajeni (chyba senzoru)
        OFF     = 3,  // napajeni vypnuto, ceka na zapnuti
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
    uint8_t              _powerResetCount   = 0;
    ds18b20_resolution_t _resolution;
    int64_t              _lastConvertTimeUs = 0;
    int64_t              _conversionDelayUs;
    bool                 _pendingReinit     = false;

    PowerState _powerState     = IDLE;
    int64_t    _powerStateTime = 0;
    int64_t    _powerDelayUs;

    std::function<void()> _onPowerOn;
    std::function<void()> _onPowerOff;

    void _reinit();
};
