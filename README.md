# Claude Monitor — AiPi-Lite

ESP32-S3 firmware for a physical Claude usage-meter gadget. Polls the Anthropic API and displays session (5h) and weekly (7d) rate-limit utilization on a 1.44" ST7735 128×128 TFT LCD.

## Quick start

```bash
# 1. Create secrets
cp main/secrets.h.example main/secrets.h
# Edit: WIFI_SSID, WIFI_PASSWORD, CLAUDE_TOKEN

# 2. Build & flash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Token push (keep the device running)

The OAuth token expires. `push_claude_token.py` runs on a nearby PC to push a fresh token to the device every 4 hours.

### How it works

1. Reads your Claude Code OAuth token from `~/.claude/.credentials.json`
2. If fewer than 2 hours remain until expiry, forces a refresh via `claude -p ping`
3. POSTs the fresh token to `http://claude-meter.local/` (the device's config endpoint)
4. The device saves it to NVS and polls immediately

### Setup (systemd timer)

```bash
# Copy script
sudo cp push_claude_token.py /usr/local/bin/
sudo chmod +x /usr/local/bin/push_claude_token.py

# Create systemd user service
mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/claude-token-push.service << 'EOF'
[Unit]
Description=Push Claude OAuth token to AiPi-Lite device
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/bin/python3 /usr/local/bin/push_claude_token.py
EOF

cat > ~/.config/systemd/user/claude-token-push.timer << 'EOF'
[Unit]
Description=Push Claude token every 4 hours

[Timer]
OnCalendar=*-*-* 00/4:00:00
Persistent=true

[Install]
WantedBy=timers.target
EOF

# Enable and start
systemctl --user daemon-reload
systemctl --user enable --now claude-token-push.timer
```

### Requirements on the PC

- Python 3
- `claude` CLI installed and authenticated (the script runs `claude -p ping` to force token refresh)
- Network access to the device at `claude-meter.local` (mDNS)

### Manual push

```bash
python3 push_claude_token.py
```

### Override device URL

Edit `DEVICE_URL` in the script to use an IP address instead of mDNS:
```python
DEVICE_URL = "http://192.168.1.42/"
```

## Hardware

| Component | Pins |
|---|---|
| ST7735 128×128 | CLK=16, MOSI=17, CS=15, DC=7, RST=18, BL=3 |
| WS2812 LED | GPIO 46 |
| Button | GPIO 42 (active-low, force poll) |
| Battery ADC | GPIO 1 (ADC1_CH1) |
| Power hold | GPIO 10 |

## Web config

`http://claude-meter.local/` — view stats, paste a new token. POST a `token=` field to update the token directly.

## Architecture

See [CLAUDE.md](CLAUDE.md) for detailed architecture notes.
