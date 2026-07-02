#!/usr/bin/env python3
"""
push_claude_token.py
Push the Claude Code OAuth access token to the Claude Meter (AiPi-Lite) device.

What it does, in order:
  1. Read the token from ~/.claude/.credentials.json.
  2. If under --margin hours remain, force a refresh: back-date expiresAt in
     the credentials file, then run `claude -p ping` so the CLI rotates the
     token through its own OAuth path (the CLI only refreshes when *it*
     considers the token expired, not at our margin — merely calling
     `claude -p ping` without back-dating is a no-op), then re-read.
     Refusal-to-push is the safety net: if the token is empty or already
     expired after the attempt, we exit WITHOUT pushing, so the device is
     never poisoned with a dead token. An under-margin-but-still-valid
     token is pushed anyway, with a warning.
  3. POST the fresh token to the device, trying mDNS first and falling back
     to a cached / explicit IP, with retries.  A successful hostname push
     caches the resolved IP for next time, so a flaky mDNS lookup no longer
     loses a whole cycle.

Usage:
    python3 push_claude_token.py                          # mDNS auto-discovery
    python3 push_claude_token.py --url http://1.2.3.4/    # explicit device URL
    python3 push_claude_token.py --ip 192.168.66.123      # mDNS + IP fallback
Env equivalents: CLAUDE_METER_URL, CLAUDE_METER_IP, CLAUDE_METER_SECRET.

Exit codes: 0 ok, 1 config/credentials error, 2 token not fresh, 3 push failed.
"""
import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from urllib.parse import urlsplit, urlunsplit

DEFAULT_DEVICE_URL = "http://claude-meter.local/"
DEFAULT_CREDS_PATH = os.path.expanduser("~/.claude/.credentials.json")
DEFAULT_MARGIN_H   = 6      # refresh if fewer than this many hours remain.
                            # Token life is ~8h and the timer runs every 4h, so
                            # 6h means almost every run pushes a near-full-life
                            # token — enough to ride out one missed cycle.
IP_CACHE_PATH      = os.path.expanduser("~/.cache/claude-meter-ip")


def log(msg):
    print(msg, flush=True)


def err(msg):
    print(msg, file=sys.stderr, flush=True)


# ----------------------------------------------------------------------------
# Credentials
# ----------------------------------------------------------------------------
def load_creds(path):
    with open(path) as f:
        return json.load(f)["claudeAiOauth"]


def remaining_hours(info):
    return (info.get("expiresAt", 0) - int(time.time() * 1000)) / 3_600_000


def refresh_via_cli():
    """Force the Claude CLI to refresh the token. Observable (we surface the
    outcome) rather than silent. Returns True if the call succeeded."""
    claude = os.environ.get("CLAUDE_BIN") or "claude"
    try:
        r = subprocess.run([claude, "-p", "ping", "--model", "haiku"],
                           capture_output=True, text=True, timeout=90)
    except FileNotFoundError:
        err(f"WARNING: '{claude}' not on PATH — cannot refresh "
            f"(set CLAUDE_BIN to the full path)")
        return False
    except Exception as e:
        err(f"WARNING: refresh call failed: {e}")
        return False
    if r.returncode != 0:
        err(f"WARNING: 'claude -p ping' exited {r.returncode}: "
            f"{(r.stderr or r.stdout or '').strip()[:200]}")
        return False
    return True


def fingerprint(token):
    return hashlib.sha256(token.encode()).hexdigest()[:8]


def _atomic_write(path, data_bytes):
    d = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(dir=d)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(data_bytes)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def force_refresh(creds_path, margin_h):
    """Force the Claude CLI to rotate the OAuth token by back-dating
    expiresAt, then letting `claude -p ping` refresh through the CLI's own
    OAuth path. Returns True if the token actually rotated."""
    for _attempt in range(2):
        st_before = os.stat(creds_path)
        orig_bytes = open(creds_path, "rb").read()
        j = json.loads(orig_bytes)
        old_token = j["claudeAiOauth"]["accessToken"]
        old_exp = j["claudeAiOauth"]["expiresAt"]

        j["claudeAiOauth"]["expiresAt"] = int(time.time() * 1000) - 60_000
        new_bytes = json.dumps(j, indent=2).encode() + b"\n"

        # Re-check mtime right before the write; if something else touched
        # the file (e.g. a live interactive session) since we read it,
        # restart from a fresh read rather than clobbering it.
        st_now = os.stat(creds_path)
        if st_now.st_mtime != st_before.st_mtime:
            continue
        _atomic_write(creds_path, new_bytes)
        break
    else:
        err("WARNING: credentials file kept changing underneath us — "
            "skipping forced refresh this cycle")
        return False

    refresh_via_cli()

    new_j = json.loads(open(creds_path, "rb").read())
    new_token = new_j["claudeAiOauth"]["accessToken"]
    new_exp = new_j["claudeAiOauth"]["expiresAt"]
    rotated = new_token != old_token and new_exp > old_exp

    if rotated:
        log(f"Rotated token {fingerprint(old_token)} -> {fingerprint(new_token)} "
            f"(new expiry {time.strftime('%F %T', time.localtime(new_exp / 1000))})")
    else:
        err("WARNING: 'claude -p ping' did not rotate the token")
        # Only restore if the file still holds our back-dated marker and the
        # old token — otherwise a concurrent legitimate refresh landed and we
        # must not clobber it.
        if new_token == old_token and new_exp == j["claudeAiOauth"]["expiresAt"]:
            _atomic_write(creds_path, orig_bytes)
            err("WARNING: restored original credentials file")

    return rotated


# ----------------------------------------------------------------------------
# IP cache (so a flaky mDNS lookup doesn't lose a cycle)
# ----------------------------------------------------------------------------
def read_cached_ip():
    try:
        with open(IP_CACHE_PATH) as f:
            return f.read().strip() or None
    except OSError:
        return None


def write_cached_ip(ip):
    try:
        os.makedirs(os.path.dirname(IP_CACHE_PATH), exist_ok=True)
        with open(IP_CACHE_PATH, "w") as f:
            f.write(ip)
    except OSError:
        pass


def host_of(url):
    return urlsplit(url).hostname


def url_with_host(url, new_host):
    parts = urlsplit(url)
    netloc = new_host + (f":{parts.port}" if parts.port else "")
    return urlunsplit((parts.scheme, netloc, parts.path or "/", "", ""))


# ----------------------------------------------------------------------------
# Push
# ----------------------------------------------------------------------------
def post_once(url, token, secret, timeout=10):
    data = urllib.parse.urlencode({"token": token}).encode()
    req = urllib.request.Request(url, data=data, method="POST")
    if secret:
        req.add_header("X-Auth", secret)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.status


def push(targets, token, secret, retries):
    """Try each target URL, each up to `retries` times. Returns the URL that
    succeeded, or None."""
    last = None
    for url in targets:
        for attempt in range(1, retries + 1):
            try:
                status = post_once(url, token, secret)
                log(f"Device responded: HTTP {status}  ({url})")
                return url
            except urllib.error.HTTPError as e:
                # The device answered — a 4xx/5xx won't fix itself on retry,
                # and both targets are the same device, so don't fall over
                # to the next target either.
                err(f"Device rejected push ({url}): HTTP {e.code} {e.reason}")
                return None
            except (urllib.error.URLError, socket.error, OSError) as e:
                reason = getattr(e, "reason", e)
                last = e
                # A name-resolution failure won't fix itself on retry — fail
                # over to the next target (the IP fallback) immediately.
                if isinstance(reason, socket.gaierror):
                    err(f"Cannot resolve {url}: {reason} — trying fallback")
                    break
                err(f"Push attempt {attempt}/{retries} to {url} failed: {reason}")
                if attempt < retries:
                    time.sleep(2 * attempt)
    return None


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", "-u",
                   default=os.environ.get("CLAUDE_METER_URL", DEFAULT_DEVICE_URL),
                   help="device URL (default: %(default)s, env: CLAUDE_METER_URL)")
    p.add_argument("--ip",
                   default=os.environ.get("CLAUDE_METER_IP"),
                   help="explicit fallback IP if the hostname won't resolve "
                        "(env: CLAUDE_METER_IP)")
    p.add_argument("--secret",
                   default=os.environ.get("CLAUDE_METER_SECRET"),
                   help="X-Auth secret if the device requires one "
                        "(env: CLAUDE_METER_SECRET)")
    p.add_argument("--creds", "-c", default=DEFAULT_CREDS_PATH,
                   help="path to credentials JSON (default: %(default)s)")
    p.add_argument("--margin", type=float, default=DEFAULT_MARGIN_H,
                   help="refresh if fewer than this many hours remain "
                        "(default: %(default)s)")
    p.add_argument("--retries", type=int, default=3,
                   help="push attempts per target (default: %(default)s)")
    return p.parse_args()


def main():
    args = parse_args()
    base_url = args.url.rstrip("/") + "/"

    try:
        info = load_creds(args.creds)
    except Exception as e:
        err(f"ERROR: cannot read credentials: {e}")
        sys.exit(1)

    rem = remaining_hours(info)
    rotated = None
    if rem < args.margin:
        log(f"Token expires in {rem:.1f}h — forcing refresh via 'claude -p ping'...")
        rotated = force_refresh(args.creds, args.margin)
        try:
            info = load_creds(args.creds)            # re-read after refresh
        except Exception as e:
            err(f"ERROR: cannot re-read credentials: {e}")
            sys.exit(1)
        rem = remaining_hours(info)                  # recompute against *now*

    token = info.get("accessToken", "")
    if not token:
        err("ERROR: accessToken is empty after refresh — not pushing")
        sys.exit(2)
    if rem < 0:
        err(f"ERROR: token still expired ({rem:.1f}h) after refresh — "
            f"not pushing a dead token")
        sys.exit(2)
    if rotated is False:
        err(f"WARNING: refresh did not rotate token; pushing existing "
            f"token with {rem:.1f}h left")

    log(f"Token {fingerprint(token)} expires "
        f"{time.strftime('%F %T', time.localtime(info.get('expiresAt', 0) / 1000))} "
        f"({rem:.1f}h)")

    # Build target list: configured host first, then a fallback IP
    # (explicit --ip, else last-known-good cached IP).
    targets = [base_url]
    host = host_of(base_url)
    fallback_ip = args.ip or read_cached_ip()
    if fallback_ip and fallback_ip != host:
        targets.append(url_with_host(base_url, fallback_ip))

    log(f"Pushing token (expires in {rem:.1f}h) to: {', '.join(targets)}")
    winner = push(targets, token, args.secret, args.retries)
    if not winner:
        err("ERROR: all push targets failed")
        sys.exit(3)

    # Cache the resolved IP of a successful hostname push for next time.
    win_host = host_of(winner)
    try:
        ip = socket.gethostbyname(win_host)
        write_cached_ip(ip)
    except OSError:
        pass


if __name__ == "__main__":
    main()
