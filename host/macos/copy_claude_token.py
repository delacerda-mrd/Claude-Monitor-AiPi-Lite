#!/usr/bin/env python3
"""
copy_claude_token.py
Force-rotate the Claude Code OAuth access token, then copy it to the clipboard
for manual pasting into the Claude Meter web page (e.g. http://192.168.66.123/).

By default every run forces a FRESH token (max lifetime on the device):
back-date expiresAt in the stored credentials, let `claude -p ping` refresh
through the CLI's own OAuth path, then copy the rotated token — same
force-expire+ping approach as push_claude_token.py (commit 7d0b2cf).

Credentials come from the macOS Keychain entry "Claude Code-credentials"
(where Claude Code stores them on macOS), falling back to
~/.claude/.credentials.json. Refuses to copy an expired token (--force to
override) so the device is never fed a dead token.

Usage:
    python3 copy_claude_token.py               # rotate, then token -> clipboard
    python3 copy_claude_token.py --no-refresh  # copy current token as-is
    python3 copy_claude_token.py --force       # copy even if expired

Exit codes: 0 ok, 1 cannot read/write credentials, 2 token expired/empty.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time

KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDS_PATH = os.path.expanduser("~/.claude/.credentials.json")


def load_creds():
    """Return (full_json_dict, source). source is 'keychain' or the file path."""
    r = subprocess.run(
        ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
        capture_output=True, text=True)
    if r.returncode == 0 and r.stdout.strip():
        raw = r.stdout.strip()
        # `security -w` hex-encodes any password that isn't a single
        # printable line; decode it back to JSON if so.
        if not raw.startswith("{"):
            raw = bytes.fromhex(raw).decode()
        return json.loads(raw), "keychain"
    with open(CREDS_PATH) as f:
        return json.load(f), CREDS_PATH


def save_creds(j, source):
    if source == "keychain":
        # MUST stay a single line: `security -w` hex-encodes multi-line
        # passwords on read, which breaks both this script and the CLI.
        data = json.dumps(j, separators=(",", ":"))
        # -U updates the existing entry in place — but only if the account
        # matches, so read it off the entry rather than assuming $USER.
        r = subprocess.run(
            ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE],
            capture_output=True, text=True, check=True)
        m = re.search(r'"acct"<blob>="([^"]*)"', r.stdout)
        acct = m.group(1) if m else os.environ.get("USER", "")
        subprocess.run(
            ["security", "add-generic-password", "-U",
             "-a", acct, "-s", KEYCHAIN_SERVICE, "-w", data],
            capture_output=True, text=True, check=True)
    else:
        data = json.dumps(j, indent=2) + "\n"
        d = os.path.dirname(source)
        fd, tmp = tempfile.mkstemp(dir=d)
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, source)
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise


def refresh_via_cli():
    claude = os.environ.get("CLAUDE_BIN") or "claude"
    try:
        r = subprocess.run([claude, "-p", "ping", "--model", "haiku"],
                           capture_output=True, text=True, timeout=90)
    except FileNotFoundError:
        print(f"WARNING: '{claude}' not on PATH — cannot refresh "
              f"(set CLAUDE_BIN to the full path)", file=sys.stderr)
        return False
    except Exception as e:
        print(f"WARNING: refresh call failed: {e}", file=sys.stderr)
        return False
    if r.returncode != 0:
        print(f"WARNING: 'claude -p ping' exited {r.returncode}: "
              f"{(r.stderr or r.stdout or '').strip()[:200]}", file=sys.stderr)
        return False
    return True


def fingerprint(token):
    return hashlib.sha256(token.encode()).hexdigest()[:8]


def force_rotate():
    """Back-date expiresAt, ping, verify the token actually rotated.
    Returns True if it rotated; restores the original expiry if it didn't."""
    j, source = load_creds()
    old = j["claudeAiOauth"]
    old_token, old_exp = old["accessToken"], old["expiresAt"]

    j["claudeAiOauth"]["expiresAt"] = int(time.time() * 1000) - 60_000
    save_creds(j, source)

    refresh_via_cli()

    new_j, _ = load_creds()
    new = new_j["claudeAiOauth"]
    rotated = new["accessToken"] != old_token and new["expiresAt"] > old_exp
    if rotated:
        exp_str = time.strftime("%F %T", time.localtime(new["expiresAt"] / 1000))
        print(f"Rotated token {fingerprint(old_token)} -> "
              f"{fingerprint(new['accessToken'])} (new expiry {exp_str})")
    else:
        # Don't leave a good token marked expired by our own back-dating.
        if new["accessToken"] == old_token:
            new_j["claudeAiOauth"]["expiresAt"] = old_exp
            save_creds(new_j, source)
        print("WARNING: token did not rotate — falling back to the stored token",
              file=sys.stderr)
    return rotated


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-refresh", action="store_true",
                    help="skip the forced rotation; copy the current token as-is")
    ap.add_argument("--force", action="store_true",
                    help="copy even if the token is expired")
    args = ap.parse_args()

    try:
        if not args.no_refresh:
            force_rotate()
        info, source = load_creds()
        info = info["claudeAiOauth"]
    except Exception as e:
        print(f"ERROR: cannot read/write credentials: {e}", file=sys.stderr)
        sys.exit(1)

    token = info.get("accessToken", "")
    if not token:
        print("ERROR: accessToken is empty", file=sys.stderr)
        sys.exit(2)

    exp_ms = info.get("expiresAt", 0)
    rem_h = (exp_ms - time.time() * 1000) / 3_600_000
    if rem_h < 0 and not args.force:
        print(f"ERROR: token expired {-rem_h:.1f}h ago and refresh failed — "
              f"re-run with --force to copy it anyway", file=sys.stderr)
        sys.exit(2)

    subprocess.run(["pbcopy"], input=token.encode(), check=True)

    exp_str = time.strftime("%F %T", time.localtime(exp_ms / 1000))
    print(f"Copied token {fingerprint(token)} to clipboard (from {source})")
    print(f"Expires {exp_str} ({rem_h:.1f}h left)")


if __name__ == "__main__":
    main()
