# Claude Meter — AiPi-Lite

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
3. POSTs the fresh token to the device's config endpoint via mDNS (`claude-meter.local`) or an explicit IP
4. The device saves it to NVS and polls immediately

### Find your device

The device advertises itself as `claude-meter.local` via mDNS. To use this auto-discovery you need mDNS resolution working on the push PC:

**Avahi (most distros):**
```bash
sudo apt install avahi-daemon libnss-mdns
# Avahi should start automatically; verify it's running:
systemctl is-active avahi-daemon
# If inactive or masked:
#   sudo systemctl unmask avahi-daemon.socket
#   sudo systemctl start avahi-daemon
ping claude-meter.local    # final check
```

**systemd-resolved (systemd-based distros):**
```bash
sudo systemd-resolve --set-mdns=yes --interface=wlan0
resolvectl query claude-meter.local    # verify
```

If mDNS isn't available, find the device's IP from your router's DHCP lease table or the serial monitor output, then pass it explicitly (see below).

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

> **Note:** The service above uses mDNS auto-discovery. If you need an explicit IP instead, replace the `ExecStart` line:
> ```
> ExecStart=/usr/bin/python3 /usr/local/bin/push_claude_token.py --url http://<DEVICE-IP>/
> ```
> Or set the environment variable: `Environment=CLAUDE_METER_URL=http://<DEVICE-IP>/`

### Requirements on the PC

- Python 3
- `claude` CLI installed and authenticated (the script runs `claude -p ping` to force token refresh)
- Network access to the device (mDNS via `claude-meter.local`, or a known IP)

### Manual push

```bash
# mDNS (auto-discovery)
python3 push_claude_token.py

# Explicit IP
python3 push_claude_token.py --url http://192.168.1.42/
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

`http://claude-meter.local/` (or the device's IP) — view stats, paste a new token. POST a `token=` field to update the token directly.

> **⚠️ Security:** The config endpoint has no authentication. Anyone on your LAN can read your usage stats or overwrite the Claude API token, which grants full access to your Claude account. On a trusted home network this is low-risk, but don't expose the device to a shared or public network.

## Architecture

See [CLAUDE.md](CLAUDE.md) for detailed architecture notes.
