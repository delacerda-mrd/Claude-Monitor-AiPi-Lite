#!/usr/bin/env python3
"""
push_claude_token.py
Reads the Claude Code OAuth token from ~/.claude/.credentials.json,
force-refreshes it via 'claude -p ping' if under REFRESH_MARGIN_H hours remain,
then POSTs the fresh token to the AIPI-Lite device's config endpoint.

Runs every 4h via systemd user timer: claude-token-push.timer
"""
import json
import time
import subprocess
import urllib.request
import urllib.parse
import sys

CREDS_PATH      = "/home/jeremy/.claude/.credentials.json"
DEVICE_URL      = "http://claude-meter.local/"   # mDNS hostname
REFRESH_MARGIN_H = 2   # force refresh if less than this many hours remain


def load_creds():
    with open(CREDS_PATH) as f:
        return json.load(f)["claudeAiOauth"]


def main():
    try:
        info = load_creds()
    except Exception as e:
        print(f"ERROR: cannot read credentials: {e}", file=sys.stderr)
        sys.exit(1)

    now_ms      = int(time.time() * 1000)
    expires_ms  = info.get("expiresAt", 0)
    remaining_h = (expires_ms - now_ms) / 3_600_000

    if remaining_h < REFRESH_MARGIN_H:
        print(f"Token expires in {remaining_h:.1f}h - forcing refresh via claude -p ping ...")
        try:
            subprocess.run(
                ["claude", "-p", "ping"],
                capture_output=True,
                timeout=60,
                check=True,
            )
        except Exception as e:
            print(f"WARNING: refresh call failed: {e}", file=sys.stderr)
        # re-read after refresh attempt
        info        = load_creds()
        expires_ms  = info.get("expiresAt", 0)
        remaining_h = (expires_ms - now_ms) / 3_600_000

    token = info.get("accessToken", "")
    if not token:
        print("ERROR: accessToken is empty", file=sys.stderr)
        sys.exit(1)

    print(f"Pushing token to {DEVICE_URL}  (expires in {remaining_h:.1f}h)")
    data = urllib.parse.urlencode({"token": token}).encode()
    req  = urllib.request.Request(DEVICE_URL, data=data, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(f"Device responded: HTTP {resp.status}")
    except Exception as e:
        print(f"ERROR: push failed: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
