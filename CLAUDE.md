# Claude Meter v2 — AiPi-Lite edition · Project Context for Claude Code

## SESSION START PROTOCOL (do this first, every session)
1. `git pull` (single-machine, but keeps the habit + backup in sync). On journal
   conflict: keep both sides' entries, merge Current Status by date.
2. Read `docs/JOURNAL.md` — the Current Status block says exactly where we are.
3. Read `docs/BRINGUP.md` when deciding what to work on next.
4. Read `docs/ARCHITECTURE.md` when the task touches firmware behavior or internals
   (flow, UI, poll, OTA, power, audio). It is the behavior-truth doc (no separate SPEC).
5. Read `docs/HARDWARE.md` / `docs/BOARD_REFERENCE.md` only when the task touches
   pins/buses/peripherals.
6. Do NOT re-derive context by exploring — these docs are the source of truth.

## SESSION END PROTOCOL ("wrap up" / "save state")
1. Update `docs/JOURNAL.md`: refresh Current Status, append a dated entry.
2. Tick `docs/BRINGUP.md` boxes for anything verified ON HARDWARE this session.
3. If behavior diverged from `docs/ARCHITECTURE.md`, update it NOW — a stale doc is a bug.
4. Commit docs updates and push to origin (source changes only if the user asked).

## What this project is
ESP32-S3 firmware for a physical Claude usage-meter gadget: polls the Anthropic API
and shows session (5h) + weekly (7d) rate-limit utilization on a 1.44" ST7735 128×128
TFT, plus RGB status LED, battery meter, and audio tones. Written from scratch (no
upstream); complete and in daily use. Single-file firmware: `main/main.c` (~1466 lines)
+ `main/audio.c`. Behavior/internals truth: `docs/ARCHITECTURE.md`.
Toolchain: **ESP-IDF v5.4.4** + `esp_lcd_st7735` + LVGL 8.4.

## Layout
- `main/main.c` — the whole firmware (boot, display+LVGL+UI, Wi-Fi, poll, web server, OTA, power).
- `main/audio.c` / `audio.h` — ES8311 codec + notification tones.
- `main/secrets.h` — Wi-Fi creds + initial token (NOT committed; see `secrets.h.example`).
- `partitions.csv` / `sdkconfig` — two-OTA 16 MB layout, no PSRAM.
- `push_claude_token.py` / `notify_listener.py` — host-side token tooling (see `README.md`).

## Build / flash / debug
```bash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```
- Serial port: `/dev/ttyACM0` (Linux, native USB-CDC). This project's only dev machine is Linux.
- OTA (post first USB flash): `curl --data-binary @build/*.bin -H 'X-Auth: <secret>' http://claude-meter.local/ota`
- If a build misbehaves after an IDF change: `idf.py fullclean`.

## Secrets
`main/secrets.h` defines `WIFI_SSID`, `WIFI_PASSWORD`, `CLAUDE_TOKEN`, optional
`CFG_AUTH_SECRET`. First boot persists the token to NVS (`cfg`/`token`); later boots
load from NVS. Token is an OAuth bearer from `~/.claude/.credentials.json`.

## Hard rules for this project
- Never guess pin numbers or I2C addresses — `docs/BOARD_REFERENCE.md` or measurement only.
- Never guess behavior — `docs/ARCHITECTURE.md` or ask.
- A bring-up item is done when verified ON HARDWARE, not when it compiles.
- Every API poll spends 1 real token and counts against rate limits — don't add polls casually.
- **All LVGL access is from the main loop**; the poll task only samples values. Never touch LVGL off the main loop.
- **Light sleep must stay OFF** (kills the SPI/display bus).
- Durable hardware facts → `docs/BOARD_REFERENCE.md`. Behavior changes → `docs/ARCHITECTURE.md`.
  Narrative → `docs/JOURNAL.md`. Keep THIS file short — offload, don't accrete.
