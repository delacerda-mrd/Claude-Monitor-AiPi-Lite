# Troubleshooting Journal — Claude Meter (AiPi-Lite)

> The *Current Status* block is always the latest truth — read it first.
> Dated entries below are append-only, newest first. Never delete entries;
> they prevent re-trying failed approaches.

---

## Current Status (updated 2026-07-09)

**Phase:** Complete & in daily use. Post-bring-up (BRINGUP Phases 1–6 essentially green);
FW_PORT_KIT adopted for ongoing maintenance/feature work.

**Known working (verified on hardware):**
- Full feature set: ST7735 display + LVGL UI, Wi-Fi/SNTP/mDNS, Anthropic API poll with
  SESSION/WEEKLY bars, WS2812 status LED, ES8311 audio tones, battery meter, button
  force-poll, screen blank/wake, config web server, OTA + auto-rollback.
- Current HEAD firmware (`a5c9cde` and later — edge-triggered alerts, bounded recv
  timeouts, battery/countdown/Wi-Fi-recovery fixes, online-notify) is **flashed and
  confirmed running** on the device (user confirmed at adoption).
- Host tooling: `push_claude_token.py` token rotation verified end-to-end 2026-07-02
  (force-expire + `claude -p ping`, HTTP 200 to device); `notify_listener.py` set up.

**Known broken / unverified:**
- None blocking. Not formally measured: heap/stack headroom under load, discrete
  power-cycle ×10 (both `[~]` in BRINGUP Phase 5/6) — no instability observed in practice.

**Next steps:**
- [ ] (open — ask user what to work on next)

**Environment notes:**
- Single dev machine: this Linux box. Serial `/dev/ttyACM0` (native USB-CDC).
- ESP-IDF **v5.4.4**. Build: `source ~/esp/esp-idf/export.sh && idf.py build`.
- Sync/backup via GitHub remote `origin`
  (github.com/delacerda-mrd/Claude-Monitor-AiPi-Lite). Single machine, so cross-machine
  conflicts are unlikely, but `git pull` at session start remains the habit.

---

## Session Log (newest first)

### 2026-07-09 (Linux) — Adopted FW_PORT_KIT
**Goal:** Retrofit the FW_PORT_KIT workflow (docs + session protocols) onto this
already-complete, in-use firmware, without touching source.
**Did:**
- Inventoried from `CLAUDE.md`, `README.md`, `main/main.c`, `sdkconfig`,
  `partitions.csv`, git history, and the two project memories.
- Flavor: **greenfield-inline** (written from scratch, no upstream; single-file
  ~1466-line firmware; agent pipeline NOT used).
- Board is **AIPI-Lite (ESP32-S3, ST7735 128×128)** — no matching kit board reference
  (kit only has the two 2.8" panels), so wrote `docs/BOARD_REFERENCE.md` from the working
  firmware + running unit. This board does NOT share the CYD/ES3C28P inversion/BGR/touch
  quirks — flagged in the board ref.
- Instantiated `docs/{BOARD_REFERENCE,HARDWARE,ARCHITECTURE,BRINGUP,JOURNAL}.md`;
  offloaded the detailed architecture/behavior prose out of CLAUDE.md into
  `docs/ARCHITECTURE.md` (played the SPEC role; a separate SPEC.md was skipped as
  busywork per the greenfield playbook's inline guidance). Merged CLAUDE.md down to the
  kit's session-protocol + pointers shape.
- Backfill: user confirmed HEAD is flashed & running, so the recent-fix items are `[x]`,
  not `[~]`. This corrects the stale `fw-review-fix-plan` memory (which said the a5c9cde
  build was never flashed) — memory updated.
**Result / evidence:** docs created; no source/build files touched. `idf.py build`
run as the adoption sanity check → **clean (exit 0)**: `claude_meter_v2.bin` =
0x148990 (~1.33 MB), 36% free in the 2 MB OTA slot; bootloader 0x5220.
**Conclusion:** Project sits post-bring-up, essentially green. Adoption is docs-only.
**Next steps:** ask the user what to work on next.
