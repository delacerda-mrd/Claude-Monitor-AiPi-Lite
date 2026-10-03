# Claude Meter v2 — AiPi-Lite edition · Project Context for Claude Code

## SESSION START PROTOCOL (do this first, every session)
1. `git pull` — keeps the checkout in sync with GitHub (the Linux PC is retired as of
   2026-10-02; the Mac is the only machine). On journal conflict: keep both sides'
   entries, merge Current Status by date.
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
ESP32-S3 firmware for a physical Claude usage-meter gadget: polls the token-free
`/api/oauth/usage` endpoint (1-token header poll as fallback) and shows session (5h) +
weekly (7d) usage on a 1.44" ST7735 128×128 TFT — rings, pace, trend, Clawd mood pet,
device page — plus RGB LED, battery, audio, web dashboard. Written from scratch (no
upstream); v2 refresh 2026-10-02. Behavior/internals truth: `docs/ARCHITECTURE.md`.
Toolchain: **ESP-IDF v5.4.4** + `esp_lcd_st7735` 0.0.1 + LVGL 8.4.0 (pinned by the
committed `dependencies.lock`).

## Layout
- `main/` — the **board** half: `main.c` (boot, LVGL driver, main loop, buttons, app_say
  policy), `ui.c` + `clawd.c` (all LVGL), `power.c`, `led.c`, `board.h` (pins incl.
  audio), `fonts/`. Map + threading: `docs/ARCHITECTURE.md`.
- **`../meter_core`** (sibling repo, since 2026-10-03) — the **shared** half, compiled
  into `main`: `audio`, `voice`, `listen`, `brain`, `usage`, `settings`, `net`, `web` +
  dashboard, `app.h`; plus all host tooling (`host/`: token push, notify listener,
  brain server, launchd install) and `tools/make_voice.py`. Its README has the board
  contract. A core change affects every meter: build each board project before committing.
- `main/secrets.h` — OPTIONAL seed for Wi-Fi/token/`CFG_AUTH_SECRET` (NOT committed).
- `partitions.csv` / `sdkconfig.defaults` — two-OTA 16 MB layout + voice/model partitions, 8 MB PSRAM
  (`sdkconfig` is generated, not committed).
- `tools/` — `idf.sh` (IDF wrapper), `shot.sh` (screenshot), `serial_log.py`,
  `gen_clawd.py` (regenerates `main/clawd_anims.h`). The voice pack is built in
  meter_core (`tools/make_voice.py` → `curl --data-binary @build/voice.bin http://claude-meter.local/voice`).

## Build / flash / debug (Mac)
```bash
tools/idf.sh build                          # NOT plain export.sh: see below
tools/idf.sh flash monitor                  # port auto-detected: /dev/cu.usbmodem*
curl --data-binary @build/claude_meter_v2.bin http://192.168.66.125/ota   # preferred
tools/shot.sh                               # PNG of what the screen shows now
```
- `tools/idf.sh` pins Homebrew `python@3.12`; plain `source export.sh` picks python 3.14
  and silently fails to find the IDF venv.
- Prefer OTA: it keeps the previous image as an automatic rollback target.
- LAN access (curl to the device) needs the sandbox off; the device answers at
  `192.168.66.125` / `claude-meter.local`. Opening the USB serial port resets the chip.
- If a build misbehaves after an IDF change: `tools/idf.sh fullclean`.

## Secrets
Wi-Fi config and the token live in NVS (Wi-Fi in the driver's `nvs.net80211`, token in
`cfg`/`token`); `secrets.h` only seeds an empty device. The token is Claude Code's
OAuth bearer — on the Mac in the Keychain item "Claude Code-credentials" — pushed by
the launchd agents (`../meter_core/host/macos/install.sh`). Don't read it yourself; the agents do.

## Hard rules for this project
- Never guess pin numbers or I2C addresses — `docs/BOARD_REFERENCE.md` or measurement only.
- Never guess behavior — `docs/ARCHITECTURE.md` or ask.
- A bring-up item is done when verified ON HARDWARE, not when it compiles.
- The usage endpoint is free, but the header fallback spends 1 real token per poll —
  keep it a fallback; don't add polls casually.
- **All LVGL access is from the main loop**; the poll task only samples values. Never touch LVGL off the main loop.
- **Light sleep must stay OFF** (kills the SPI/display bus).
- Durable hardware facts → `docs/BOARD_REFERENCE.md`. Behavior changes → `docs/ARCHITECTURE.md`.
  Narrative → `docs/JOURNAL.md`. Keep THIS file short — offload, don't accrete.
