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
- [x] Heap headroom measured under load (v2, 2026-10-02): `/api/status` heap stats —
  ~113 KB free; min 91–100 KB across TLS polls + OTA uploads, 59 KB while hammering `/api/screen.bmp` (32 KB of strips each) — was min 13 KB
  before `LV_MEM_CUSTOM` + mbedTLS dynamic buffers)
- [x] Poll cadence honored (120 s USB / 300 s battery)

## Phase 6 — Validation
- [x] Long soak: runs continuously in daily use without reset (observed pre-adoption)
- [~] Formal power-cycle ×10 test — not run as a discrete test; cold-boots reliably in practice
- [x] Recovery: USB reflash always works (never rolled back — recovery path)
- [x] Token rotation end-to-end verified 2026-07-02 (host push → device HTTP 200 → NVS → poll)

## Phase 7 — v2 refresh (2026-10-02, Mac)
Verified via serial log, `/api/status` and `/api/screen.bmp` (exact panel input).
`[~]` = needs a human at the device (eyes on the panel, hands on the button).

### Toolchain / deploy
- [x] ESP-IDF v5.4.4 on the Mac (python@3.12 venv) builds v2 clean, zero warnings; `tools/idf.sh`
- [x] OTA from the Mac (`curl --data-binary`) × 9; every image self-confirmed after 15 s online
- [x] Component versions pinned: `dependencies.lock` committed (LVGL 8.4.0, st7735 0.0.1, mdns 1.13.1, led_strip 3.0.3, es8311 1.0.0~1)
### Network / data
- [x] Boots without `secrets.h`: rejoins Wi-Fi from the driver's NVS config
- [x] Token-free poll: `session=42% weekly=15% via usage api`
- [x] Usage endpoint 429 (seen with an expired token) → header fallback → 401 → `token?` + host notify
- [x] Mac launchd push agent → device `HTTP 200`; device learns `notify_ip` = the Mac
- [x] Device → Mac online-notify (inbound :5555) → listener runs push → `HTTP 200`
- [x] History survives a reboot (RTC RAM): `restored 1 history samples`
- [~] Setup AP + join-QR + captive portal — not exercised (would need the stored Wi-Fi gone)
- [~] 24 h soak on v2 (token rotation across the 4 h agent cycle)
### UI (rendering verified by screenshot; panel output needs eyes)
- [x] Splash → RINGS, PACE, TREND, CLAWD, SYSTEM all render with live data; no overlaps
- [x] Clawd animates (3 distinct frames in 5 captures)
- [x] Remote button (`POST /api/button`) cycles pages; `page` reported in `/api/status`
- [~] **Panel colors correct with `LV_COLOR_16_SWAP`** (sage/amber/red, coral, cream text) — confirm by eye
- [~] Physical button: tap = next page, hold ≥0.6 s = refresh (+ tick), press while blank = wake only
- [~] Backlight fade-in at boot; blank after 3 min, wake on press
- [~] New tones (window reset, setup) and the volume / mute / quiet-hours settings
### Board deep-dive (v2.1, 2026-10-02)
- [x] PSRAM: 8 MB octal enabled — `/api/status` hw.psram = 8388608, heap 8.4 MB
- [x] LCD at 40 MHz (stock clock) renders cleanly (screenshots)
- [x] Left button = GPIO1 active-low (live capture 02:19:18)
- [x] GPIO8 = USB power sense (unplug → 0, replug → 1); now drives ext_power
- [x] GPIO21 = charger CHRG (0 after replug, back to 1 on the full cell); status bar plug/bolt
- [~] Left-button hold 3 s on battery → power off, and left button powers back on
- [~] Battery mode now actually engages on unplug (15 % backlight, 300 s polls)
### Voice (v2.1)
- [x] USB flash with the new partition table; token + Wi-Fi survived (NVS offset unchanged)
- [x] `POST /voice` installs the Daniel pack (2.4 MB in 15 s): `voice pack "Daniel": 127 clips`
- [x] `POST /api/say` status runs with no audio errors in the log
- [~] **Speech is audible and clean** (no buzz at the end, sensible volume) — confirm by ear
- [~] Hold-to-refresh announces; event phrases (threshold, token, reset) fire
- [~] On battery: 15 % backlight, 300 s polls, battery icon level
