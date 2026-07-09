# Hardware Reference — Claude Meter (AIPI-Lite)

Thin per-project summary. **Full pin truth lives in [`BOARD_REFERENCE.md`](BOARD_REFERENCE.md)** —
this file only carries the one-line map and the per-unit facts specific to the
assembled meter. Fix durable hardware corrections in `BOARD_REFERENCE.md` first,
then reflect anything session-relevant here.

## Board / SoC
- **AIPI-Lite** (xuanzhi-yuanzhi ESP32-S3): ESP32-S3, 16 MB flash (DIO/80 MHz), **no PSRAM**.
- Console: native USB-Serial-JTAG → `/dev/ttyACM0` on Linux. `idf.py flash` auto-enters download over CDC.
- Toolchain: ESP-IDF **v5.4.4**.

## Pin map (summary — authoritative table in BOARD_REFERENCE.md §10)
| Function | GPIO | Notes |
|----------|------|-------|
| Display (ST7735 128×128) | SCLK16 MOSI17 CS15 DC7 RST18 BL3 | SPI 27 MHz; inversion OFF; BL = LEDC PWM |
| WS2812 LED | 46 | single pixel; strapping pin, OK as output post-boot |
| Button | 42 | active-low, ISR, force-poll / screen-wake |
| Battery ADC | 2 | ADC1_CH1, 12-bit / 11 dB, divider |
| Power hold | 10 | drive high to stay alive on battery |
| ES8311 codec (I2C) | SDA5 SCL4 | |
| Speaker amp enable | 9 | |
| I2S audio | MCLK6 BCLK14 WS12 DOUT11 | |

## Per-unit facts (specific to THIS assembled meter)
- **Battery ADC calibration:** the `{adc_raw, pct}` lookup table in `batt_update()`
  (`main/main.c`) was calibrated on this unit's divider. Recalibrate if the board or
  cell changes — see BOARD_REFERENCE.md §8.
- **Light sleep OFF** is mandatory (kills the SPI/display bus) — see BOARD_REFERENCE.md §12.

## Flash / partitions
Two-OTA layout on 16 MB (`partitions.csv`): `ota_0`/`ota_1` 2 MB each, `otadata`,
`nvs` kept at its original 0x9000/0x6000 offset so a saved token survives a re-flash.
Details + rollback behavior in [`ARCHITECTURE.md`](ARCHITECTURE.md).
