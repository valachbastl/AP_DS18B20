# AP_DS18B20

DS18B20 1-Wire temperature sensor driver for ESP-IDF with EWA software filter, error detection and power reset management.

## Features

- Uses Espressif managed components `espressif/onewire_bus` + `espressif/ds18b20`
- Auto-detection of up to 8 sensors on one bus
- EWA (Exponential Weighted Average) software filter per sensor
- Configurable measurement resolution (9–12 bit)
- Internal conversion timing – no `vTaskDelay` needed in user code
- Error detection: CRC errors, out-of-range values, 85°C power-on reset value, temperature jump filter
- Power reset state machine with `onPowerOn` / `onPowerOff` callbacks
- Full bus re-initialization (`_reinit`) after power restore – handles stale RMT handles
- Distinguishes between sensor not found (`NO_SENSOR_TEMP = -126`) and read error (`INVALID_TEMP = -127`)

## Dependencies

Add to your project's `idf_component.yml`:

```yaml
dependencies:
  espressif/onewire_bus: ">=0.1.0"
  espressif/ds18b20: ">=0.1.0"
  idf: ">=5.0.0"
```

## Installation

### PlatformIO

Add to `platformio.ini`:

```ini
lib_deps =
    https://github.com/valachbastl/AP_DS18B20.git
```

Or with specific version:

```ini
lib_deps =
    https://github.com/valachbastl/AP_DS18B20.git#v1.0.0
```

## Usage

### Initialization

```cpp
#include "AP_DS18B20.h"

AP_DS18B20 ds18b20({
    .gpio_num    = GPIO_NUM_4,
    .onPowerOn   = []() { /* enable power to sensor */ },
    .onPowerOff  = []() { /* disable power to sensor */ },
});
```

### Configuration options

```cpp
AP_DS18B20::Config cfg;
cfg.gpio_num         = GPIO_NUM_4;          // 1-Wire GPIO pin
cfg.powerDelayMs     = 5000;                // min delay between power state transitions [ms]
cfg.filterWeight     = 80;                  // EWA weight for new value, 0–100
cfg.errorIgnoreCount = 3;                   // consecutive errors to ignore before accepting bad value
cfg.maxPowerResets   = 3;                   // max power reset cycles
cfg.maxTempJump      = 10.0f;               // max allowed temperature jump between readings [°C]
cfg.resolution       = DS18B20_RESOLUTION_12B;  // 9–12 bit (94/188/375/750 ms conversion)
cfg.onPowerOn        = nullptr;             // callback: called when power is turned ON
cfg.onPowerOff       = nullptr;             // callback: called when power is turned OFF
```

### Reading temperature

Call from a periodic task loop. The order matters:

```cpp
// 1. Run power reset state machine (~100 ms period or faster)
ds18b20.tick();

// 2. Read result of previous conversion
float temp;
ds18b20.getTempFiltered(0, temp);  // index 0 = first sensor

// 3. Trigger next conversion (internally rate-limited to conversion time)
ds18b20.convertAll();

// Check result
if (temp == AP_DS18B20::NO_SENSOR_TEMP) {
    // sensor not found during enumeration
} else if (temp == AP_DS18B20::INVALID_TEMP) {
    // sensor found but read failed
} else {
    // valid temperature in °C
}
```

### Multiple sensors

```cpp
uint8_t count = ds18b20.getSensorCount();
for (uint8_t i = 0; i < count; i++) {
    float temp;
    ds18b20.getTempFiltered(i, temp);
}
```

## API Reference

| Method | Description |
|--------|-------------|
| `AP_DS18B20(config)` | Constructor – initializes 1-Wire bus and enumerates sensors |
| `convertAll()` | Trigger temperature conversion on all sensors (internally rate-limited) |
| `getTempFiltered(index, temperature)` | Read filtered temperature for sensor at index |
| `tick()` | Run power reset state machine – call periodically (~100 ms) |
| `getSensorCount()` | Returns number of sensors found during enumeration |

| Constant | Value | Description |
|----------|-------|-------------|
| `NO_SENSOR_TEMP` | -126.0f | Sensor not found (index out of range or not enumerated) |
| `INVALID_TEMP` | -127.0f | Sensor found but read failed |

## Author

Petr Adámek
