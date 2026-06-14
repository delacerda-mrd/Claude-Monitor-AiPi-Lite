#!/usr/bin/env python3
"""
push_claude_token.py
Reads the Claude Code OAuth token from ~/.claude/.credentials.json,
force-refreshes it via 'claude -p ping' if under REFRESH_MARGIN_H hours remain,
then POSTs the fresh token to the AIPI-Lite device's config endpoint.

Usage:
    python3 push_claude_token.py                           # mDNS auto-discovery
    python3 push_claude_token.py --url http://1.2.3.4/      # explicit IP
    CLAUDE_METER_URL=http://1.2.3.4/ python3 push_claude_token.py

Runs every 4h via systemd user timer: claude-token-push.timer
"""
import argparse
import json
import os
import time
import subprocess
import urllib.request
import urllib.parse
import sys

DEFAULT_DEVICE_URL = "http://claude-meter.local/"
DEFAULT_CREDS_PATH = os.path.expanduser("~/.claude/.credentials.json")
REFRESH_MARGIN_H   = 2   # force refresh if less than this many hours remain


def load_creds(path):
    with open(path) as f:
        return json.load(f)["claudeAiOauth"]


def parse_args():
    p = argparse.ArgumentParser(
        description="Push a fresh Claude OAuth token to an AIPI-Lite device")
    p.add_argument("--url", "-u",
                   default=os.environ.get("CLAUDE_METER_URL", DEFAULT_DEVICE_URL),
                   help="Device URL (default: %(default)s, env: CLAUDE_METER_URL)")
    p.add_argument("--creds", "-c",
                   default=DEFAULT_CREDS_PATH,
                   help="Path to Claude credentials JSON (default: ~/.claude/.credentials.json)")
    return p.parse_args()


def main():
    args = parse_args()
    device_url = args.url.rstrip("/") + "/"

    try:
        info = load_creds(args.creds)
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
        info        = load_creds(args.creds)
        expires_ms  = info.get("expiresAt", 0)
        remaining_h = (expires_ms - now_ms) / 3_600_000

    token = info.get("accessToken", "")
    if not token:
        print("ERROR: accessToken is empty", file=sys.stderr)
        sys.exit(1)

    print(f"Pushing token to {device_url}  (expires in {remaining_h:.1f}h)")
    data = urllib.parse.urlencode({"token": token}).encode()
    req  = urllib.request.Request(device_url, data=data, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(f"Device responded: HTTP {resp.status}")
    except Exception as e:
        print(f"ERROR: push failed: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
