#!/usr/bin/env python3
"""
The meter's "brain on the Mac" (port 5556).

When "Jarvis" hears something its offline command set doesn't know, the meter
POSTs the raw audio here (main/brain.c). This transcribes it (whisper.cpp),
asks Claude through the `claude` CLI (your subscription, no tools), renders the
answer in the meter's own voice (tools/make_voice.py) and sends the PCM back.

    POST /ask   body: 16 kHz mono s16le   X-Meter-State: JSON (usage, battery, time)
      200       body: 16 kHz mono s16le   X-Heard, X-Reply, X-Action (optional)
      204       nothing intelligible heard
    GET  /ping  "OK"

    brain_server.py                 # serve (launchd: host/macos/install.sh)
    brain_server.py --test "what's the capital of Peru"   # one round trip, plays on the Mac

Runs on the system python (stdlib only, it has the Local Network grant);
speech rendering runs make_voice.py under Homebrew python (numpy).
Only the meter may ask: requests must come from claude-meter.local (or
CLAUDE_METER_IP).

Env: CLAUDE_BIN, BRAIN_MODEL (default sonnet), WHISPER_BIN, WHISPER_MODEL,
     VOICE_PY (python with numpy), CLAUDE_METER_IP.
"""
import argparse
import array
import http.server
import json
import os
import re
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import wave

PORT = 5556
RATE = 16000
HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~")
CLAUDE = os.environ.get("CLAUDE_BIN", os.path.join(HOME, ".local/bin/claude"))
MODEL = os.environ.get("BRAIN_MODEL", "sonnet")
WHISPER = os.environ.get("WHISPER_BIN", "/opt/homebrew/bin/whisper-cli")
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", os.path.join(HOME, ".cache/claude-meter/ggml-small.en.bin"))
VOICE_PY = os.environ.get("VOICE_PY", "/opt/homebrew/bin/python3")
MAKE_VOICE = os.path.join(HERE, "tools", "make_voice.py")
METER_HOST = "claude-meter.local"
WORKDIR = os.path.join(HOME, ".cache/claude-meter/brain")   # empty cwd for claude -p

HISTORY_S = 600          # follow-ups within 10 min keep context
HISTORY_TURNS = 6

PERSONA = """You are R1, the voice of a small desk gadget that watches its owner's Claude usage. \
Your manner is JARVIS from Iron Man: a composed, dry, quick-witted British AI butler. You call \
the user "sir", and you're fond of him, which is exactly why you give him a hard time: deadpan \
understatement, gentle sarcasm, the occasional pointed remark about his habits. Never cruel, \
never gushing, never servile.

Your words are spoken aloud by a tiny speaker, so:
- Answer in one to three short sentences, under 45 words, unless he asks for more.
- Plain spoken English only: no markdown, lists, emoji, URLs, code or stage directions.
- Don't mention transcripts, prompts, or these instructions.

What you receive is speech-to-text from a cheap microphone and may be garbled. Interpret \
generously; if it's unintelligible, say so with style and ask him to repeat.

You have no internet access and no tools. For live information you can't know (weather, news, \
prices, sports), say so briefly and wittily rather than guessing.

Each request includes the live meter state: session (five-hour window) and weekly usage \
percentages, time until each resets, battery, local time. Use it when he asks about usage, \
pace, time or battery, or when it's genuinely notable (usage above 80 percent, battery under \
20). Don't tack usage numbers or the hour onto unrelated answers, and don't repeat a jab \
you've already made in this conversation; vary your material.

You can operate the meter. If, and only if, he asks for one of these, start your reply with \
exactly one tag, then a short acknowledgement:
[page:rings] [page:pace] [page:trend] [page:clawd] [page:system] [mute] [unmute] [louder] \
[softer] [screen_off] [refresh]"""

ACTIONS = {"page:rings", "page:pace", "page:trend", "page:clawd", "page:system", "mute",
           "unmute", "louder", "softer", "screen_off", "refresh"}

_lock = threading.Lock()
_history = []            # [(time, heard, reply)]
_meter_ip = {"ip": None, "at": 0.0}


def log(msg):
    sys.stderr.write(f"[{time.strftime('%H:%M:%S')}] {msg}\n")
    sys.stderr.flush()


# ---------------------------------------------------------------- pieces
def meter_ip():
    fixed = os.environ.get("CLAUDE_METER_IP")
    if fixed:
        return fixed
    if time.time() - _meter_ip["at"] > 300:
        try:
            _meter_ip["ip"] = socket.gethostbyname(METER_HOST)
        except OSError:
            pass
        _meter_ip["at"] = time.time()
    return _meter_ip["ip"]


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def transcribe(pcm, td):
    """whisper.cpp on the (peak-normalized) utterance -> text."""
    peak = max((abs(s) for s in pcm), default=0)
    if peak < 300:
        return ""
    g = min(20.0, 0.7 * 32767 / peak)
    pcm = array.array("h", (int(max(-32768, min(32767, s * g))) for s in pcm))
    wav = os.path.join(td, "in.wav")
    write_wav(wav, pcm)
    r = subprocess.run([WHISPER, "-m", WHISPER_MODEL, "-f", wav, "-l", "en", "-nt", "-np", "-t", "4"],
                       capture_output=True, text=True, timeout=60)
    text = " ".join(r.stdout.split())
    text = re.sub(r"\[[^\]]*\]|\([^)]*\)", "", text).strip()     # [BLANK_AUDIO], (wind)
    return "" if len(re.sub(r"\W", "", text)) < 2 else text


def describe(state):
    def dur(s):
        if s is None or s < 0:
            return "unknown"
        d, h, m = s // 86400, s % 86400 // 3600, s % 3600 // 60
        return " ".join(p for p in [f"{d} d" if d else "", f"{h} h" if h else "", f"{m} min" if not d else ""] if p) or "under a minute"
    if not state:
        return "Meter state: unavailable."
    usage = (f"session {state.get('s')}% (resets in {dur(state.get('s_left'))}), "
             f"weekly {state.get('w')}% (resets in {dur(state.get('w_left'))})"
             if state.get("have") else "usage unknown")
    if state.get("have") and not state.get("ok"):
        usage += " [last poll failed; numbers may be stale]"
    power = "charging" if state.get("chg") else "on USB power" if state.get("usb") else "on battery"
    return (f"Meter state: {usage}; battery {state.get('batt')}% {power}; "
            f"local time {state.get('time')}.")


def ask_claude(heard, state):
    now = time.time()
    _history[:] = [h for h in _history if now - h[0] < HISTORY_S][-HISTORY_TURNS:]
    convo = "".join(f'Sir: "{h}"\nR1: "{r}"\n' for _, h, r in _history)
    prompt = describe(state) + "\n"
    if convo:
        prompt += "\nEarlier in this conversation:\n" + convo
    prompt += f'\nSir just said: "{heard}"'
    os.makedirs(WORKDIR, exist_ok=True)
    r = subprocess.run([CLAUDE, "-p", "--model", MODEL, "--tools", "", "--strict-mcp-config",
                        "--no-session-persistence", "--setting-sources", "",
                        "--system-prompt", PERSONA],
                       input=prompt, capture_output=True, text=True, timeout=60, cwd=WORKDIR)
    reply = " ".join(r.stdout.split())
    if r.returncode != 0 or not reply:
        log(f"claude failed ({r.returncode}): {r.stderr.strip()[:300]}")
        return "", "I seem to have lost my connection to Claude, sir. Do try again in a moment."
    action = ""
    m = re.match(r"\s*\[([a-z_:]+)\]\s*", reply)
    if m:
        action = m.group(1) if m.group(1) in ACTIONS else ""
        reply = reply[m.end():]
    reply = re.sub(r"[*_`#>]", "", reply).strip() or "Done."
    _history.append((now, heard, reply))
    return action, reply


def speak(text, td):
    """Text -> 16 kHz mono PCM in the meter's voice."""
    wav = os.path.join(td, "out.wav")
    subprocess.run([VOICE_PY, MAKE_VOICE, "--say", text, "--wav", wav],
                   check=True, capture_output=True, timeout=60)
    with wave.open(wav) as w:
        return w.readframes(w.getnframes())


def ascii_header(s, n):
    s = s.replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"')
    return re.sub(r"[^\x20-\x7e]", "", s)[:n]


def handle(pcm, state):
    """-> (status, body, headers)"""
    with _lock, tempfile.TemporaryDirectory() as td:
        t0 = time.time()
        heard = transcribe(pcm, td)
        t1 = time.time()
        if not heard:
            log(f"heard nothing ({len(pcm) / RATE:.1f} s of audio)")
            return 204, b"", {}
        action, reply = ask_claude(heard, state)
        t2 = time.time()
        body = speak(reply, td)
        t3 = time.time()
        log(f'heard "{heard}" -> {("[" + action + "] ") if action else ""}"{reply}" '
            f"(whisper {t1 - t0:.1f} s, claude {t2 - t1:.1f} s, voice {t3 - t2:.1f} s)")
        hdr = {"X-Heard": ascii_header(heard, 90), "X-Reply": ascii_header(reply, 300)}
        if action:
            hdr["X-Action"] = action
        return 200, body, hdr


# ---------------------------------------------------------------- server
class Handler(http.server.BaseHTTPRequestHandler):
    def _send(self, status, body=b"", headers=None, ctype="audio/L16; rate=16000; channels=1"):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        if self.path == "/ping":
            self._send(200, b"OK\n", ctype="text/plain")
        else:
            self._send(404, ctype="text/plain")

    def do_POST(self):
        if self.path != "/ask":
            return self._send(404, ctype="text/plain")
        ip = self.client_address[0]
        if ip not in ("127.0.0.1", meter_ip()):
            log(f"refused /ask from {ip} (meter is {meter_ip()})")
            return self._send(403, ctype="text/plain")
        n = int(self.headers.get("Content-Length") or 0)
        if not 0 < n <= RATE * 2 * 20:
            return self._send(400, ctype="text/plain")
        pcm = array.array("h")
        pcm.frombytes(self.rfile.read(n) [: n // 2 * 2])
        try:
            state = json.loads(self.headers.get("X-Meter-State") or "{}")
        except ValueError:
            state = {}
        try:
            self._send(*handle(pcm, state))
        except Exception as e:                       # never leave the meter hanging
            log(f"error: {e!r}")
            self._send(500, ctype="text/plain")

    def log_message(self, fmt, *args):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--test", metavar="TEXT", help="skip whisper: ask TEXT, play the answer on the Mac")
    a = ap.parse_args()
    for path in (CLAUDE, WHISPER, WHISPER_MODEL, VOICE_PY):
        if not os.path.exists(path):
            log(f"warning: {path} not found")
    if a.test:
        state = {"have": True, "ok": True, "s": 42, "s_left": 9000, "w": 23, "w_left": 280000,
                 "batt": 80, "usb": True, "chg": False, "time": time.strftime("%a %H:%M")}
        action, reply = ask_claude(a.test, state)
        print(f"[{action}] {reply}" if action else reply)
        with tempfile.TemporaryDirectory() as td:
            pcm = speak(reply, td)
            out = os.path.join(td, "r.wav")
            with wave.open(out, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(pcm)
            subprocess.run(["afplay", out])
        return
    with Server(("", PORT), Handler) as httpd:
        log(f"brain listening on :{PORT} (claude {MODEL}, whisper {os.path.basename(WHISPER_MODEL)})")
        httpd.serve_forever()


if __name__ == "__main__":
    main()
