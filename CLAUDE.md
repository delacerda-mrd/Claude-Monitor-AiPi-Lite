# Claude Meter v2 — AiPi-Lite edition

ESP32-S3 firmware for a physical Claude usage-meter gadget. Polls the Anthropic API and displays session (5h) and weekly (7d) rate-limit utilization on a 1.44" ST7735 128×128 TFT LCD, plus an RGB status LED and battery meter.

## Hardware

- **MCU:** ESP32-S3 (target: `esp32s3`)
- **Display:** ST7735, 128×128, SPI (CLK=16, MOSI=17, CS=15, DC=7, RST=18, BL=3)
- **LED:** WS2812 addressable RGB on GPIO 46 (1 pixel)
- **Button:** GPIO 42 active-low (force poll on press)
- **Power hold:** GPIO 10 (keeps board alive on battery)
- **Battery:** ADC1 channel 1 (GPIO 1) via voltage divider. 12-bit, 11 dB attenuation. Lookup table in `batt_update()` maps raw ADC to percentage for a single-cell LiPo (3.2–4.2 V). The table was calibrated on an AIPI-Lite board; different divider resistor values or ADC reference variance will shift readings. Recalibrate by measuring battery voltage at known charge levels and adjusting the `{adc_raw, pct}` entries.
- **USB detect:** `usb_serial_jtag_is_connected()` — SOF monitor, polled in main loop
- **Board model:** AIPI-Lite / xuanzhi-yuanzhi-esp32s3

## Build system

ESP-IDF v5.4. The top-level `CMakeLists.txt` is a standard IDF project file. The `main/` component pulls in:
- `teriyakigod/esp_lcd_st7735` ^0.0.1
- `lvgl/lvgl` ^8.4.0
- `espressif/led_strip` ^3.0.3
- `espressif/mdns` ^1.11.1

All dependencies are locked in `dependencies.lock` and live under `managed_components/`.

### Build & flash

```bash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Replace `/dev/ttyACM0` with the actual serial port.

## Secrets

`main/secrets.h` holds Wi-Fi credentials and the initial Claude API token:

```c
#define WIFI_SSID     "..."
#define WIFI_PASSWORD "..."
#define CLAUDE_TOKEN  "..."
```

This file is **not** committed. At first boot the token is persisted to NVS (`cfg` namespace, key `token`). Subsequent boots load from NVS.

## Architecture

`main/main.c` (~930 lines) is the entire firmware. Key flow:

1. **`app_main()`** — power-hold GPIO 10, init NVS, load/save token, PM config (40–160 MHz), init LED, display+LVGL+UI, Wi-Fi, SNTP, mDNS, config HTTP server, button ISR, then LVGL + poll loop.
2. **Poll task** (`poll_task`) — adjusts backlight via `usb_serial_jtag_is_connected()`, reads battery ADC, fires a tiny POST to `https://api.anthropic.com/v1/messages` with `max_tokens:1` just to read the rate-limit headers.
3. **LVGL UI** — two bars (SESSION and WEEKLY) with percentage labels and countdown-to-reset text. Bar color: green (<60%), amber (<85%), red (≥85%). Battery percentage at bottom. ERR label only shown on error.
4. **Config web server** — HTTP on port 80 at `http://claude-meter.local/`. Shows stats + token paste form. POST saves to NVS and triggers immediate re-poll.
5. **LED** — dimmed values: blue at boot, then green/amber/red matching the worse of the two usage percentages. Red on API error.
6. **Backlight** — LEDC PWM on GPIO 3, 5 kHz, 8-bit. 80% on USB, 20% on battery. Adjusts in poll loop via USB detect.
7. **Power** — ESP-PM at 40–160 MHz, light sleep disabled (kills SPI bus).

## Key constraints

- Anthropic API call uses a real token and **counts against rate limits** (1 token/poll). Poll interval: 120 s.
- Token is OAuth bearer from `~/.claude/.credentials.json` field `claudeAiOauth.accessToken`. Requires `anthropic-beta: oauth-2025-04-20` header.
- Token stored as raw string in NVS; web form URL-decodes form-encoded POST bodies.
- LVGL uses a 1 ms tick timer (`esp_timer`) and double-buffered partial refresh (16 rows per buffer — 4 KB each).
- Font: UNSCII 8 (pixel-perfect bitmap, no anti-aliasing — crisp on low-res ST7735).
- Display `invert_color=false`, `set_gap(0,0)`, `swap_xy=true`, `mirror(true,false)`, 27 MHz SPI clock.
- `push_claude_token.py` pushes fresh tokens to the device via mDNS (`claude-meter.local`) or an explicit `--url`; uses systemd timer for 4h cadence.
