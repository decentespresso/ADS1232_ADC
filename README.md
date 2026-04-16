# ADS1232_ADC

Thread-safe Arduino/ESP32 library for the [ADS1232](https://www.ti.com/product/ADS1232) 24-bit analog-to-digital converter, designed for load cell and weight scale applications.

Features:

- FreeRTOS background sampling task (no polling needed)
- Mutex-protected data access — safe to read from any task/core
- Ring buffer with configurable averaging window and outlier rejection
- Multi-instance support (multiple load cells on one ESP32)
- Calibration and tare helpers
- Power-down / power-up control

Based on [HX711_ADC](https://github.com/olkal/HX711_ADC) by Olav Kallhovd. Original ADS1232 port by Sofronio Chen.

## Installation

### PlatformIO

Add to `platformio.ini`:

```ini
lib_deps =
    https://github.com/decentespresso/ADS1232_ADC.git
```

### Arduino IDE

Download this repository as a ZIP and install via **Sketch > Include Library > Add .ZIP Library**.

## Wiring

| ADS1232 Pin | ESP32 GPIO | Description |
|-------------|------------|-------------|
| DOUT        | any input  | Data output (active low when ready) |
| SCLK        | any output | Serial clock |
| PDWN        | any output | Power down (HIGH = on, LOW = off) |
| A0          | any output | Channel select (optional, pass `255` to skip) |

Connect load cell excitation and signal lines to the ADS1232 analog inputs per the [datasheet](https://www.ti.com/lit/ds/symlink/ads1232.pdf).

## Quick Start

### Background Sampling (recommended)

```cpp
#include <ADS1232_ADC.h>

// ADS1232_ADC(dout, sck, pdwn, a0, samples, ignoreHighest, ignoreLowest)
ADS1232_ADC scale(11, 12, 13, 255, 16, true, true);

void setup() {
    Serial.begin(115200);

    scale.begin();                    // Initialize pins, power up
    scale.start(2000, true);          // 2s stabilization + tare
    scale.setCalFactor(1234.56);      // Set calibration factor
    scale.beginTask(100);             // Background sampling every 100ms
}

void loop() {
    float weight = scale.getData();   // Thread-safe, returns smoothed value
    Serial.printf("Weight: %.2f g\n", weight);
    delay(200);
}
```

### Polling Mode

For simpler setups without FreeRTOS tasks:

```cpp
#include <ADS1232_ADC.h>

ADS1232_ADC scale(11, 12, 13, 255, 16, true, true);

void setup() {
    Serial.begin(115200);

    scale.begin();
    scale.start(2000, true);
    scale.setCalFactor(1234.56);
}

void loop() {
    if (scale.update()) {             // Returns 1 when new data was read
        float weight = scale.getData();
        Serial.printf("Weight: %.2f g\n", weight);
    }
}
```

### Dual Load Cells

```cpp
ADS1232_ADC scale1(11, 12, 13, 255, 16, true, true);
ADS1232_ADC scale2(47, 48,  9, 255, 16, true, true);

void setup() {
    scale1.begin();
    scale1.start(2000, true);
    scale1.setCalFactor(1234.56);
    scale1.beginTask(100);

    scale2.begin();
    scale2.start(2000, true);
    scale2.setCalFactor(2345.67);
    scale2.beginTask(100);
}

void loop() {
    float w1 = scale1.getData();
    float w2 = scale2.getData();
    Serial.printf("Scale 1: %.2f g  Scale 2: %.2f g\n", w1, w2);
    delay(200);
}
```

## Calibration

1. Start with no load on the scale, call `tare()`
2. Place a known mass on the scale
3. Call `getNewCalibration(known_mass)` — returns and applies the new factor
4. Store the calibration factor (e.g. in NVS/EEPROM) and restore with `setCalFactor()` on boot

```cpp
// During calibration:
scale.tare();
// ... place known weight ...
float cal = scale.getNewCalibration(100.0);  // 100g reference weight
Serial.printf("Calibration factor: %.2f\n", cal);

// On subsequent boots:
scale.setCalFactor(cal);
```

## API Reference

### Constructor

```cpp
ADS1232_ADC(uint8_t dout, uint8_t sck, uint8_t pdwn,
            uint8_t a0 = 255,       // Channel select pin (255 = unused)
            int samples = 64,        // Averaging window size (max 256)
            bool ignHigh = true,     // Reject highest sample in average
            bool ignLow = true);     // Reject lowest sample in average
```

### Lifecycle

| Method | Description |
|--------|-------------|
| `begin()` | Initialize GPIO pins, power up the ADC |
| `begin(uint8_t gain)` | Initialize with specific gain setting |
| `start(unsigned long t)` | Stabilize for `t` ms + tare |
| `start(unsigned long t, bool dotare)` | Stabilize, optionally tare |
| `beginTask(uint32_t intervalMs)` | Start FreeRTOS background sampling task |
| `end()` | Stop background task, release resources |

### Data Access (thread-safe)

| Method | Description |
|--------|-------------|
| `getData()` | Returns smoothed, calibrated weight value |
| `tare()` | Blocking tare (up to 2s timeout) |
| `tareNoDelay()` | Non-blocking tare (captures current average as offset) |
| `getTareStatus()` | Check if tare is complete |
| `setCalFactor(float cal)` | Set calibration factor |
| `getCalFactor()` | Get current calibration factor |
| `getNewCalibration(float known_mass)` | Calculate and apply new calibration factor |

### Sampling Control

| Method | Description |
|--------|-------------|
| `setSamplesInUse(int samples)` | Change averaging window (resets buffer) |
| `update()` | Manual poll (no-op when background task running) |
| `refreshDataSet()` | Fill buffer with fresh readings (blocking, no-op when task running) |

### Hardware Control

| Method | Description |
|--------|-------------|
| `powerDown()` | Power down the ADC |
| `powerUp()` | Power up the ADC |
| `setGain(uint8_t gain)` | Set gain (affects clock pulse count) |
| `setChannelInUse(int channel)` | Select ADC channel (0 or 1) |
| `getChannelInUse()` | Get current channel |

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).
