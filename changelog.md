# Changelog

## [1.1.0] - 2026-06-11

### Added
- Inicializace `begin()` – vytvori 1-Wire sbernici a zenumeruje senzory, vraci `esp_err_t` (konstruktor uz nesaha na hardware ani neabortuje aplikaci pri chybe)

### Changed
- **BREAKING:** `begin()` je nyni nutne zavolat pred prvnim ctenim / `getSensorCount()`. Konstruktor uz sbernici nevytvari ani neenumeruje
- Licence zmenena na MIT (drive UNLICENSED)

### Refactored
- Sjednoceni vytvoreni sbernice + enumerace do privatniho helperu `_initBus()` (sdileno `begin()` a `_reinit()`) – odstranena duplicita ~35 radku; chovani recovery (`_reinit`) i state machine napajeni beze zmeny

## [1.0.0] - 2026-02-20

### Added
- Inicializace 1-Wire sbernice pres ESP-IDF managed components (`espressif/onewire_bus` + `espressif/ds18b20`)
- Autodekce az 8 senzoru DS18B20 na jedne sbernici
- Konfigurovatelne rozliseni mereni (9–12 bit, `ds18b20_resolution_t`)
- EWA (Exponential Weighted Average) softwarovy filtr na sensor
- Interni casovani konverze – bez `vTaskDelay` v uzivatelskem kodu (`readPending` + `_lastConvertTimeUs`)
- Detekce chyb: CRC chyba, hodnota pod -125 °C, power-on reset hodnota 85 °C, filtr skoku teploty (`maxTempJump`)
- State machine resetu napajeni: IDLE → ON → OFF_REQ → OFF → ON
- Callbacky `onPowerOn` / `onPowerOff` (typ `std::function<void()>`) pro rizeni napajeni
- Plna reinicializace sbernice po obnove napajeni (`_reinit`) – obnovuje RMT handles a znovu enumeruje senzory
- Rozliseni `NO_SENSOR_TEMP = -126.0f` (senzor nenalezen) vs `INVALID_TEMP = -127.0f` (chyba cteni)
