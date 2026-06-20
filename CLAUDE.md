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

Optionally define `CFG_AUTH_SECRET` in `secrets.h` to require an `X-Auth: <secret>` header on the mutating HTTP endpoints (`POST /` and `POST /ota`). Left undefined, those endpoints are open (legacy behavior).

## Architecture

`main/main.c` (~930 lines) is the entire firmware. Key flow:

1. **`app_main()`** — power-hold GPIO 10, init NVS, load/save token, PM config (40–160 MHz), init LED, display+LVGL+UI, Wi-Fi, SNTP, mDNS, config HTTP server, button ISR, then LVGL + poll loop.
2. **Poll task** (`poll_task`) — adjusts backlight via `usb_serial_jtag_is_connected()`, reads battery ADC, fires a tiny POST to `https://api.anthropic.com/v1/messages` with `max_tokens:1` just to read the rate-limit headers.
3. **LVGL UI** — two bars (SESSION and WEEKLY) with percentage labels and countdown-to-reset text. Bar color: green (<60%), amber (<85%), red (≥85%). Battery percentage at bottom. Status label shows `TOKEN?` on an auth failure (401/403/stale token) vs `ERR` on a network/API failure (`poll_result_t`). **All LVGL access is from the main loop** — the poll task only samples values (`g_batt_pct`, `g_usage`); the main loop renders them (`ui_update`/`ui_render_battery`).
4. **Config web server** — HTTP on port 80 at `http://claude-meter.local/`. `GET /` shows stats (incl. firmware version + active OTA slot) + token paste form. `POST /` URL-decodes the `token` field (reads the full body via a recv loop), saves to NVS, and triggers immediate re-poll. `POST /ota` streams a raw firmware `.bin` into the inactive OTA slot, verifies it, sets the boot partition, and reboots. Both POSTs honor the optional `CFG_AUTH_SECRET`.
5. **LED** — dimmed values: blue at boot, then green/amber/red matching the worse of the two usage percentages. Red on API error.
6. **Backlight / screen** — LEDC PWM on GPIO 3, 5 kHz, 8-bit. ~80% on USB, ~15% on battery (`BL_DUTY_USB`/`BL_DUTY_BATT`). All backlight + panel on/off is owned by the main loop's screen manager (`screen_tick`/`screen_set`/`apply_backlight`); `power_eval()` only sets the power state it reads. The screen blanks (backlight off + panel `disp_off`) after `SCREEN_TIMEOUT_S` (180 s) idle on both USB and battery; it wakes on button press, an API error, or any change in usage (`g_screen_wake`). Only the on-brightness differs by power source. When blanked, the first button tap only wakes the screen (no forced poll/beep); once on, a tap forces a poll with feedback (the ISR branches on `g_screen_on`).
7. **Power** — ESP-PM at 40–160 MHz, light sleep disabled (kills SPI bus). `power_eval()` applies battery-saving settings on transitions: `WIFI_PS_MAX_MODEM` (vs `MIN_MODEM` on USB) and a longer poll interval. Audio (ES8311 + MCLK) is suspended when idle and resumed only for playback.

## Key constraints

- Anthropic API call uses a real token and **counts against rate limits** (1 token/poll). Poll interval: 120 s on USB (`POLL_INTERVAL_USB_S`), 300 s on battery (`POLL_INTERVAL_BATT_S`).
- Token is OAuth bearer from `~/.claude/.credentials.json` field `claudeAiOauth.accessToken`. Requires `anthropic-beta: oauth-2025-04-20` header.
- Token stored as raw string in NVS (guarded by `g_usage_mutex` since the poll task reads it while the HTTP task may rewrite it); web form URL-decodes form-encoded POST bodies.
- **Partitions:** two-OTA layout on 16 MB flash (`partitions.csv`: `ota_0`/`ota_1`, 2 MB each, `otadata`). NVS stays at `0x9000`/`0x6000` so a saved token survives a re-flash. First flash after the layout change must be over USB; subsequent updates can go over Wi-Fi via `POST /ota`.
- **Auto-rollback:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. OTA images boot `PENDING_VERIFY`; the main loop calls `esp_ota_mark_app_valid_cancel_rollback()` (`ota_confirm_image`) after `OTA_SELFTEST_S` (15 s) of stable online operation, else the next reset rolls back to the previous slot. The Wi-Fi-failure path intentionally skips confirmation. The stats page exposes the live state (`ota_state_str`). Rollback support is in the **bootloader**, so it only takes effect after a USB flash (OTA doesn't update the bootloader); USB flashes are never rolled back (recovery path).
- LVGL uses a 1 ms tick timer (`esp_timer`) and double-buffered partial refresh (16 rows per buffer — 4 KB each).
- Font: UNSCII 8 (pixel-perfect bitmap, no anti-aliasing — crisp on low-res ST7735).
- Display `invert_color=false`, `set_gap(0,0)`, `swap_xy=true`, `mirror(true,false)`, 27 MHz SPI clock.
- `push_claude_token.py` pushes fresh tokens via mDNS (`claude-meter.local`) with a cached/explicit-IP fallback (`--ip`, `~/.cache/claude-meter-ip`) and retries; refreshes via `claude -p ping` when under `--margin` (default 6h) and **refuses to push an expired token**. Optional `--secret`/`CLAUDE_METER_SECRET` for `CFG_AUTH_SECRET`. Runs on a systemd user timer (4h cadence); the unit sets `CLAUDE_BIN` so `claude` resolves under `systemctl --user`.
