# Bring-up Checklist — Claude Meter (AIPI-Lite)

The "nothing missed" document, tailored to the AIPI-Lite board. A box is `[x]` ONLY
when verified on hardware (evidence noted), never merely when it compiles.

Status legend: `[ ]` not started · `[~]` in progress / unverified-on-HW · `[x]` verified on HW

> **Adoption note (2026-07-09):** this project was already complete and in daily use
> when FW_PORT_KIT was adopted. Items below are backfilled from the running unit. The
> device is confirmed running the current HEAD firmware (commit `a5c9cde` and later),
> flashed and observed by the user — so the recent fixes are marked `[x]`, not `[~]`.

## Phase 0 — Groundwork
- [x] Board pin list captured — `docs/BOARD_REFERENCE.md` (from working firmware + running unit)
- [x] Peripheral ICs identified (ST7735, WS2812, ES8311) — datasheets not stored, but bring-up proves them
- [x] ESP-IDF v5.4.4 installed on the dev machine (this Linux box), version recorded in CLAUDE.md
- n/a Upstream firmware baseline — greenfield, no upstream

## Phase 1 — Toolchain, boot, console
- [x] Project builds clean (`idf.py build`) — device is running a real build
- [x] Flash works over USB (`idf.py -p /dev/ttyACM0 flash`); native USB-CDC download
- [x] Console visible on `/dev/ttyACM0`
- [x] 16 MB flash, DIO/80 MHz detected at boot
- n/a External RAM — no PSRAM on this board
- [x] CPU 40–160 MHz ESP-PM scaling; no brownout issues on USB or battery
- [x] Boot reaches `app_main` with no watchdog resets at idle

## Phase 2 — Pin audit
- [x] Every pin cross-checked against `BOARD_REFERENCE.md` §10 (from working firmware)
- [x] Strapping pins noted: GPIO46 (WS2812) works as output post-boot
- [x] No pin conflicts (button 42, battery ADC 2, power-hold 10 all distinct)
- [x] Active-low button captured; backlight/LED polarity captured

## Phase 3 — Buses
- [x] I2C (SDA5/SCL4): ES8311 codec responds (audio plays)
- [x] SPI (display): ST7735 @ 27 MHz stable
- [x] I2S (MCLK6/BCLK14/WS12/DOUT11): audio output clean
- [x] ADC1_CH1 (GPIO2): battery voltage reads plausibly

## Phase 4 — Peripherals, one at a time
### Display (ST7735 128×128)
- [x] Init produces stable image, no noise/tearing
- [x] Inversion OFF is correct (black is black) — `invert_color=false`
- [x] Colors correct (no BGR swap needed)
- [x] Orientation correct at app rotation (`swap_xy`, `mirror(true,false)`)
- [x] Sustained LVGL partial refresh without artifacts
- [x] Backlight PWM (GPIO3): ~80% USB / ~15% battery; blank-on-idle + wake works
### Input
- [x] Button (GPIO42): force-poll when screen on; wake-only when blanked
- n/a Touch — board has none
### RGB LED (WS2812, GPIO46)
- [x] Boot blue → green/amber/red by worst usage; red on error
### Audio (ES8311 + speaker)
- [x] Codec responds, amp enable (GPIO9) polarity correct
- [x] Notification tones clean at speaker (boot arpeggio, ticks, threshold/error tones)
### Battery (ADC1_CH1, GPIO2)
- [x] Reads across charge range; `{adc_raw,pct}` table calibrated on this unit
### Wi-Fi / SNTP / mDNS
- [x] Connects, gets time via SNTP, advertises `claude-meter.local`
- [x] Late-connect recovery: periodic `esp_wifi_connect()` + `esp_restart()` on success (a5c9cde) — confirmed flashed & running
### API poll
- [x] POST to api.anthropic.com reads rate-limit headers; SESSION/WEEKLY bars update
- [x] `TOKEN?` on auth failure vs `ERR` on network failure; alerts edge-triggered (a5c9cde)
- [x] Online-notify GET/POST to `NOTIFY_HOST_URL` on `POLL_AUTH` (a5c9cde) — host listener receives it
### Config web server
- [x] `GET /` stats page; `POST /` token update (URL-decoded, saved to NVS, re-poll)
- [x] Optional `CFG_AUTH_SECRET` X-Auth gate on POST / and POST /ota
### OTA
- [x] `POST /ota` streams to inactive slot, verifies, flips boot partition, reboots
- [x] Auto-rollback: image self-confirms after 15 s stable online, else next reset reverts

## Phase 5 — Integration
- [x] All peripherals run simultaneously (display + Wi-Fi + audio + LED + battery) in daily use
- [~] Heap/stack headroom measured under load — not explicitly measured; no observed instability
- [x] Poll cadence honored (120 s USB / 300 s battery)

## Phase 6 — Validation
- [x] Long soak: runs continuously in daily use without reset (observed pre-adoption)
- [~] Formal power-cycle ×10 test — not run as a discrete test; cold-boots reliably in practice
- [x] Recovery: USB reflash always works (never rolled back — recovery path)
- [x] Token rotation end-to-end verified 2026-07-02 (host push → device HTTP 200 → NVS → poll)
