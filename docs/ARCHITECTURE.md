# Architecture & Behavior — Claude Meter (AiPi-Lite)

Behavior + internals truth for this firmware. (No separate `SPEC.md`: this is an
inline, complete project written from scratch — this file plays the SPEC role. If
implemented behavior diverges from what's written here, update THIS file in the same
session — a stale doc is a bug.) Hardware pins live in
[`HARDWARE.md`](HARDWARE.md) / [`BOARD_REFERENCE.md`](BOARD_REFERENCE.md); host-side
tooling in the top-level `README.md`.

## What it does
ESP32-S3 gadget that polls the Anthropic API and shows session (5h) and weekly (7d)
rate-limit utilization on a 1.44" ST7735 128×128 TFT, plus an RGB status LED, battery
meter, and notification tones. `main/main.c` (~1466 lines) + `main/audio.c` (~482) are
the whole firmware.

## Flow

1. **`app_main()`** — power-hold GPIO 10, init NVS, load/save token, PM config
   (40–160 MHz), init LED, display+LVGL+UI, Wi-Fi, SNTP, mDNS, config HTTP server,
   button ISR, then the LVGL + poll loop.
2. **Poll task** (`poll_task`) — adjusts backlight via `usb_serial_jtag_is_connected()`,
   reads battery ADC, fires a tiny POST to `https://api.anthropic.com/v1/messages` with
   `max_tokens:1` purely to read the rate-limit headers.
3. **LVGL UI** — two bars (SESSION, WEEKLY) with percentage labels and
   countdown-to-reset text. Bar color: green (<60%), amber (<85%), red (≥85%). Battery %
   at bottom. Status label shows `TOKEN?` on auth failure (401/403/stale) vs `ERR` on
   network/API failure. **All LVGL access is from the main loop** — the poll task only
   samples values (`g_batt_pct`, `g_usage`); the main loop renders them
   (`ui_update`/`ui_render_battery`).
4. **Config web server** — HTTP port 80 at `http://claude-meter.local/`. `GET /` shows
   stats (incl. firmware version + active OTA slot) + token paste form. `POST /`
   URL-decodes the `token` field (full body via recv loop), saves to NVS, triggers an
   immediate re-poll. `POST /ota` streams a raw firmware `.bin` into the inactive OTA
   slot, verifies, sets boot partition, reboots. Both POSTs honor the optional
   `CFG_AUTH_SECRET` (`X-Auth` header).
5. **LED** — dimmed: blue at boot, then green/amber/red = worse of the two usage %; red
   on API error.
6. **Backlight / screen manager** — LEDC PWM on GPIO 3, 5 kHz, 8-bit. ~80% on USB, ~15%
   on battery (`BL_DUTY_USB`/`BL_DUTY_BATT`). All backlight + panel on/off is owned by the
   main loop's screen manager (`screen_tick`/`screen_set`/`apply_backlight`); `power_eval()`
   only reads the power state. Screen blanks (backlight off + panel `disp_off`) after
   `SCREEN_TIMEOUT_S` (180 s) idle on both USB and battery; wakes on button press, API
   error, or any usage change (`g_screen_wake`). When blanked, the first button tap only
   wakes the screen (no poll/beep); once on, a tap forces a poll with feedback (the ISR
   branches on `g_screen_on`).
7. **Power** — ESP-PM 40–160 MHz, light sleep disabled (kills SPI bus). `power_eval()`
   applies battery-saving on transitions: `WIFI_PS_MAX_MODEM` (vs `MIN_MODEM` on USB) and
   a longer poll interval. Audio (ES8311 + MCLK) suspended when idle, resumed only for
   playback.
8. **Online-notify** — on a `POLL_AUTH` failure the firmware fires a fire-and-forget
   GET/POST to `NOTIFY_HOST_URL` (`http://shadowtrooper.local:5555/notify`, `main/main.c`)
   so the host's `notify_listener.py` can push a fresh token immediately instead of
   waiting for the 4h timer. Error alerts are edge-triggered (fire on entering an error
   state, not every poll).

## Audio (`main/audio.c`)
ES8311 + speaker play tones for key events; routine poll successes are silent.
Boot=C–E–G rising arpeggio · button=short C7 tick · token-pushed=C–F · cross 60%=G–C ·
cross 85%=three staccato beeps · API error=E–C descending.

## Key constraints
- API call uses a **real token and counts against rate limits** (1 token/poll). Poll
  interval: 120 s on USB (`POLL_INTERVAL_USB_S`), 300 s on battery (`POLL_INTERVAL_BATT_S`).
- Token is OAuth bearer from `~/.claude/.credentials.json` field
  `claudeAiOauth.accessToken`; requires `anthropic-beta: oauth-2025-04-20` header.
- Token stored as raw string in NVS (`cfg` namespace, key `token`), guarded by
  `g_usage_mutex` (poll task reads while the HTTP task may rewrite). Web form URL-decodes
  form-encoded bodies.
- **Partitions / OTA:** two-OTA on 16 MB, NVS kept at original offset so a saved token
  survives re-flash. First flash after the layout change must be over USB; later updates
  can go over Wi-Fi via `POST /ota`.
- **Auto-rollback:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. OTA images boot
  `PENDING_VERIFY`; the main loop calls `esp_ota_mark_app_valid_cancel_rollback()`
  (`ota_confirm_image`) after `OTA_SELFTEST_S` (15 s) of stable online operation, else the
  next reset rolls back. The Wi-Fi-failure path intentionally skips confirmation. Rollback
  lives in the **bootloader**, so it only takes effect after a USB flash; USB flashes are
  never rolled back (recovery path).
- LVGL: 1 ms tick timer (`esp_timer`), double-buffered partial refresh (16 rows/buffer,
  4 KB each). Font: UNSCII 8 (pixel-perfect bitmap, crisp on low-res ST7735).
- Display: `invert_color=false`, `set_gap(0,0)`, `swap_xy=true`, `mirror(true,false)`,
  27 MHz SPI.

## Host-side tooling (see README.md)
- `push_claude_token.py` — pushes fresh tokens via mDNS with cached/explicit-IP fallback;
  force-expires + `claude -p ping` to rotate when under `--margin`; refuses to push an
  expired token. systemd user timer, `00/4:05` cadence.
- `notify_listener.py` — listens on `NOTIFY_HOST_URL`, runs `push_claude_token.py` on demand.
