# Changelog

## [1.2.0] - 2026-10-30

### Added
- `getAddress(index)` - 64bitova ROM adresa cidla na indexu (0 = index mimo rozsah), napr. pro vypis cidel a prirazeni roli v aplikaci.
- `getTempByAddress(address, temperature)` - jako `getTempFiltered()`, ale vybira cidlo podle ROM adresy; zustava spravne i pri posunu indexu. Cidlo, ktere na sbernici prave neni: `ESP_ERR_NOT_FOUND` + `NO_SENSOR_TEMP`.

### Fixed
- `maxPowerResets` pri vice cidlech neomezoval nic: pocitadlo resetu bylo spolecne a nulovalo ho uspesne cteni *kterehokoli* cidla. Vadne cidlo vedle zdraveho tak spoustelo power-reset cele sbernice porad dokola. Pocitadlo je ted per cidlo (nuluje ho jen jeho vlastni uspesne cteni, pri reinitu se prenasi podle ROM adresy); napajeni zustava spolecne. U jednoho cidla beze zmeny.
- Stav filtru (EWA, chyby) se pri reinitu sbernice prirazuje podle ROM adresy, ne podle pozice. Drive pri vice cidlech po zmizeni jednoho a posunu indexu pokracoval filtr indexu s hodnotami jineho cidla (`maxTempJump` pak nove hodnoty nekolik cyklu zahazoval).
- `errorIgnoreCount = 255` vedl k deleni nulou (pad): delitel `errorIgnoreCount + 1` se pocital v `uint8_t` a pretekl na 0. Pocita se 16bitove.
- Neplatna hodnota `resolution` v `Config` cetla mimo tabulku konverznich casu. Hodnota mimo rozsah se omezi na 12 bit.
- `CMakeLists.txt`: `REQUIRES` uvadel neexistujici komponentu `esp_log` (komponenta logu se v ESP-IDF jmenuje `log`) - cisty ESP-IDF build (`idf.py`) selhal pri konfiguraci na "Failed to resolve component". Opraveno na `log`.
- `idf_component.yml` (a priklad v README): spodni meze komponent `>=0.1.0` neodpovidaly pouzitemu API - `ds18b20_new_device_from_enumeration()` a `ds18b20_trigger_temperature_conversion_for_all()` existuji az od `espressif/ds18b20` 0.2.0, `espressif/onewire_bus` zacina na 1.0.0 a ESP-IDF 6 podporuje od 1.0.3. Opraveno na `ds18b20 >=0.2.0`, `onewire_bus >=1.0.3`.

### Changed
- Start bez zbytecne druhe enumerace: kdyz `begin()` najde aspon jedno cidlo (napajeni zjevne je), stavovy automat prejde rovnou do `ON` a `onPowerOn` se zavola z `begin()`. Drive prvni `tick()` po `powerDelayMs` znovu zavolal `onPowerOn` a cele sbernici udelal reinit (druha enumerace). Kdyz `begin()` nic nenajde, chovani beze zmeny (po `powerDelayMs` `onPowerOn` + reinit - cidla mohou byt jen bez napajeni).
- Dokumentace `getTempFiltered()` odpovida chovani: `NO_SENSOR_TEMP` znamena "zadna hodnota" - senzor nenalezen / index mimo rozsah **nebo jeste zadne mereni** (napr. hned po `begin()` ci resetu napajeni); `INVALID_TEMP` pri selhanem cteni bez dobre hodnoty k drzeni (chyby nad `errorIgnoreCount` nebo selhavani od zacatku). Hlavicka drive chybne uvadela `INVALID_TEMP` i pro "jeste zadne mereni". Chovani beze zmeny.
- Konstruktor je `explicit` (zadna implicitni konverze z `Config`). Prima inicializace `AP_DS18B20 ds({...})` beze zmeny.
- README: "non-aborting" misto "non-blocking" init (`begin()` enumeruje sbernici a blokuje), doplnena poznamka o vlastnictvi instance jednim taskem (bez interniho zamku) a o posunu indexu pri vice cidlech po reinitu (cteni podle adresy).
- Komentare v kodu a texty `ESP_LOG*` prelozeny do anglictiny. Logika beze zmeny.
- `.gitignore` doplnen o `TODO_next_version.md` (lokalni pracovni poznamky).

## [1.1.1] - 2026-06-16

### Fixed
- `idf_component.yml` melo `version: 1.0.0`, nesouhlasilo s `library.json` (1.1.0) - srovnano. Bez zmeny kodu (jen metadata).

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
