# AIPI-Lite (xuanzhi-yuanzhi ESP32-S3) Board Reference

Complete hardware reference for the **AIPI-Lite** dev board as used by the Claude
Meter firmware.
Compiled from: this project's working firmware (`main/main.c`, `sdkconfig`,
`partitions.csv`) + a physically assembled, running unit (photo `aipilite_cm.webp`).

Manufacturer: xuanzhi-yuanzhi (玄知远知) — AIPI-Lite ESP32-S3 module board.
SKU / PCB model: AIPI-Lite / xuanzhi-yuanzhi-esp32s3.

**Rules for filling this in:** never guess a pin — a visible `TBD` is correct, a
guess is a bug. Provenance tags: `(firmware)` = taken from working firmware
constants, `(verified on hardware YYYY-MM-DD)` = observed on a running unit,
`(datasheet)`. This board is NOT one of the kit's 2.8" panels — the ST7735 128×128
here does NOT need inversion ON and is wired quite differently; do not carry the
CYD/ES3C28P quirks over.

---

## 1. SoC
| Parameter | Value |
|-----------|-------|
| Module / chip | ESP32-S3 `(firmware: CONFIG_IDF_TARGET=esp32s3)` |
| PSRAM | **None enabled** — `CONFIG_SPIRAM` is not set. Firmware runs entirely in internal SRAM. `(firmware)` |
| External flash | 16 MB, DIO mode @ 80 MHz `(firmware: CONFIG_ESPTOOLPY_FLASHSIZE=16MB)` |
| USB | Native USB-CDC (USB-Serial-JTAG). `usb_serial_jtag_is_connected()` is used as the "on external power" signal. `(firmware, verified on hardware)` |
| CPU clock | 160 MHz max, scales 40–160 MHz via ESP-PM (`CONFIG_PM_ENABLE=y`, light sleep OFF — it kills the SPI bus). `(firmware)` |

## 2. USB / Serial
| Parameter | Value |
|-----------|-------|
| Bridge IC or native | Native USB-Serial-JTAG (no bridge chip) `(firmware, verified)` |
| Linux device | `/dev/ttyACM0` (native CDC → `ttyACM*`) `(verified on hardware)` |
| macOS device | `/dev/cu.usbmodem*` if ever used from a Mac (not this project's machine) |
| Download-mode entry | Standard ESP32-S3 (hold BOOT/GPIO0 while resetting) — rarely needed; `idf.py flash` triggers auto-download over CDC. |

## 3. Display
| Parameter | Value |
|-----------|-------|
| Size / type | 1.44" TFT, 128×128 `(verified on hardware)` |
| Driver IC | ST7735 (via `teriyakigod/esp_lcd_st7735` component) `(firmware)` |
| Interface / SPI / clock | SPI, 27 MHz pixel clock (`LCD_PIXEL_CLK`) `(firmware)` |
| Inversion required? | **NO** — `esp_lcd_panel_invert_color(panel, false)`. `(firmware, verified)` |
| Orientation flags | `swap_xy(true)`, `mirror(true,false)`, `set_gap(0,0)` `(firmware, verified)` |
| RGB order | Default (no BGR swap needed) `(firmware, verified)` |

### Display GPIO Mapping `(firmware, verified on hardware)`
| Signal | GPIO | Notes |
|--------|------|-------|
| SCLK | 16 | |
| MOSI | 17 | |
| CS | 15 | |
| DC | 7 | |
| RST | 18 | |
| BL | 3 | LEDC PWM, 5 kHz, 8-bit. ~80% duty on USB, ~15% on battery. |

## 4. Touch
**None.** This board has no touchscreen. Sole input is one push button.

## 5. Input — Button
| Parameter | Value |
|-----------|-------|
| Button | GPIO 42, active-low, GPIO ISR. Forces a poll (or wakes a blanked screen). `(firmware, verified)` |

## 6. RGB LED
| Parameter | Value |
|-----------|-------|
| Part / bus | WS2812 addressable (via `espressif/led_strip`), single pixel `(firmware)` |
| Pin | GPIO 46 `(firmware, verified)` |
| Use | Dimmed status: blue at boot, then green/amber/red = worse of the two usage %; red on API error. |

## 7. Audio — ES8311 codec + speaker
| Parameter | Value |
|-----------|-------|
| Codec | ES8311 on I2C: SDA=5, SCL=4 `(firmware, verified)` |
| Speaker amp enable | GPIO 9 `(firmware, verified)` |
| I2S | MCLK=6, BCLK=14, WS=12, DOUT=11 `(firmware, verified)` |
| Notes | MCLK + codec suspended when idle, resumed only for playback (power saving). Plays notification tones on threshold crossings / errors. See `main/audio.c`. |

## 8. Battery
| Parameter | Value |
|-----------|-------|
| Sense | ADC1 channel 1 = **GPIO 2** on ESP32-S3, via voltage divider. 12-bit, 11 dB atten. `(firmware, verified — GPIO corrected in commit d50b320)` |
| Cell | Single-cell LiPo, 3.2–4.2 V range. |
| Calibration | `{adc_raw, pct}` lookup table in `batt_update()`, calibrated on THIS AIPI-Lite unit. Different divider resistors / ADC variance shift readings — recalibrate per unit. `(per-unit, verified on hardware)` |

## 9. Power hold
| Parameter | Value |
|-----------|-------|
| Hold pin | GPIO 10 — driven high in `app_main` to keep the board powered on battery (latching power circuit). `(firmware, verified)` |

## 10. Complete GPIO Allocation Table `(firmware)`
| GPIO | Function | Notes |
|------|----------|-------|
| 2 | Battery ADC (ADC1_CH1) | input, analog |
| 3 | Display backlight | LEDC PWM |
| 4 | I2C SCL (ES8311) | |
| 5 | I2C SDA (ES8311) | |
| 6 | I2S MCLK | |
| 7 | Display DC | |
| 9 | Speaker amp enable | active per codec bring-up |
| 10 | Power hold | drive high to stay alive on battery |
| 11 | I2S DOUT | |
| 12 | I2S WS | |
| 14 | I2S BCLK | |
| 15 | Display CS | |
| 16 | Display SCLK | |
| 17 | Display MOSI | |
| 18 | Display RST | |
| 42 | Button | active-low, ISR |
| 46 | WS2812 LED | single pixel |

**Strapping/boot pins on ESP32-S3:** GPIO0, 45, 46. GPIO46 is used for the WS2812
LED here — it's a strapping pin (ROM message printing) but usable as output after
boot; the firmware drives it without issue `(verified on hardware)`.

## 11. Memory / flash map
- 16 MB flash, two-OTA layout (`partitions.csv`):
  - `nvs` @ 0x9000, 0x6000 (kept at original offset so a saved token survives re-flash)
  - `otadata` @ 0xf000, `phy_init` @ 0x11000
  - `ota_0` @ 0x20000, 2 MB · `ota_1` @ 0x220000, 2 MB
- No PSRAM — all runtime buffers in internal SRAM (LVGL uses two 4 KB partial-refresh buffers, 16 rows each).

## 12. Known Quirks
- **Not a 2.8" kit board.** ST7735 128×128, inversion OFF, no touch, native USB —
  none of the CYD/ES3C28P inversion/BGR/touch-bus quirks apply. Added 2026-07-09.
- **Light sleep must stay OFF** — enabling ESP-PM light sleep kills the SPI bus and
  the display goes dark/garbled. `(firmware comment, verified pre-adoption)`
- **GPIO46 (WS2812) is a strapping pin** but works as an output post-boot. Added 2026-07-09.
- **Battery ADC is GPIO 2, not GPIO 1** — an earlier hardware table listed GPIO1;
  corrected in commit d50b320. Added 2026-07-09.

---

## Characterization Checklist status (all done on the running unit, pre-adoption)
Display, LED, button, battery ADC, audio codec, Wi-Fi, USB detect and OTA were all
verified on a physically assembled unit before this reference was written (the device
is in daily use). Values above are therefore `(verified on hardware)` unless marked
otherwise. Fed back into the kit 2026-07-19 (master copy:
`FW_PORT_KIT/boards/AIPI-Lite.md`; this file is the project copy — keep the
two in sync).
