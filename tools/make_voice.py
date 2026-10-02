#!/usr/bin/env python3
"""Build the meter's voice pack with macOS text-to-speech.

    tools/make_voice.py [--voice Daniel] [--rate 180] [--style jarvis|plain] [--out build/voice.bin]
    tools/make_voice.py --audition "Session at forty two percent."   # hear it on the Mac

Upload to a running meter (no USB needed):
    curl --data-binary @build/voice.bin http://claude-meter.local/voice

Every clip is rendered by `say` at 16 kHz / 16-bit mono (the codec's rate, so the
device never resamples), trimmed of leading/trailing silence, peak-normalized and
G.711 mu-law encoded (8 bits/sample). The device strings clips together into
sentences; see main/voice.c for the script format.

Pack layout (little-endian):
    "CMVP" u16 version=1  u16 count  u32 sample_rate  char voice[24]       (36 B)
    count x { char name[20]  u32 offset  u32 length }                      (28 B each)
    mu-law sample data (offsets are from the start of the pack)
"""
import argparse
import array
import os
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 16000          # the codec runs at 16 kHz (esp-sr's rate) since v2.2

# Numbers as whole words for natural prosody: "#42" -> clip "42".
NUM_WORDS = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
             "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
             "seventeen", "eighteen", "nineteen"]
TENS = {20: "twenty", 30: "thirty", 40: "forty", 50: "fifty", 60: "sixty",
        70: "seventy", 80: "eighty", 90: "ninety"}


def number_words(n):
    if n < 20:
        return NUM_WORDS[n]
    if n == 100:
        return "one hundred"
    t, o = divmod(n, 10)
    return TENS[t * 10] + ("" if o == 0 else " " + NUM_WORDS[o])


# name -> spoken text. Names are what device scripts reference (max 19 chars).
PHRASES = {
    # building blocks
    "session_at":  "Session at",
    "weekly_at":   "Weekly at",
    "percent":     "percent.",
    "resets_in":   "resets in",
    "back_in":     "Back in",
    "day":         "day",
    "days":        "days",
    "hour":        "hour",
    "hours":       "hours",
    "minute":      "minute",
    "minutes":     "minutes",
    "and":         "and",
    "heads_up":    "Heads up.",
    "warning":     "Warning.",
    # whole sentences
    "online":      "Claude Meter online.",
    "token_new":   "New token received.",
    "token_bad":   "My token expired. Asking your Mac for a fresh one.",
    "offline":     "Connection lost.",
    "back":        "Back online.",
    "limit":       "You've hit the limit.",
    "fresh":       "Fresh window. Usage reset.",
    "setup":       "Setup mode. Scan the code on my screen to connect.",
    "ahead":       "You're ahead of pace.",
    "on_pace":     "You're on pace.",
    "room":        "Plenty of room.",
    "test":        "This is how I sound.",
    # voice control (v2.2)
    "yes":            "Yes?",
    "sorry":          "Sorry, I didn't catch that.",
    "refreshing":     "Checking now.",
    "session_resets": "Your session resets in",
    "weekly_resets":  "Your week resets in",
    "quiet_on":       "Quiet mode on.",
    "quiet_off":      "Sound is back on.",
    "louder":         "Louder.",
    "softer":         "Quieter.",
    "night":          "Goodnight.",
    "its":            "It's",
    "oclock":         "o'clock.",
    "oh":             "oh",
    "am":             "A M.",
    "pm":             "P M.",
    "welcome":        "You're welcome.",
    "intro":          "I'm R1. I keep an eye on your Claude usage.",
    "battery_at":     "Battery at",
    "charging_now":   "and charging.",
}
for _n in range(101):
    PHRASES[str(_n)] = number_words(_n)


def render(text, voice, rate, path):
    subprocess.run(["say", "-v", voice, "-r", str(rate),
                    f"--data-format=LEI16@{RATE}", "-o", path, text], check=True)
    with wave.open(path) as w:
        assert w.getframerate() == RATE and w.getsampwidth() == 2 and w.getnchannels() == 1
        pcm = array.array("h", w.readframes(w.getnframes()))
    return pcm


def trim_and_normalize(pcm, thresh=500, pad=int(RATE * 0.015), peak=0.89):
    idx = [i for i, s in enumerate(pcm) if abs(s) > thresh]
    if not idx:
        return array.array("h")
    a, b = max(idx[0] - pad, 0), min(idx[-1] + pad, len(pcm))
    pcm = pcm[a:b]
    m = max(abs(s) for s in pcm) or 1
    g = peak * 32767 / m
    return array.array("h", (int(max(-32768, min(32767, s * g))) for s in pcm))


def style_jarvis(pcm):
    """A faint synthetic 'AI' sheen: two short delayed copies (9 and 16 ms)
    make a light comb/chorus, like a voice through a small studio room or an
    intercom. Subtle by design -- it should read as polished, not robotic."""
    d1, d2 = int(RATE * 0.009), int(RATE * 0.016)
    out = array.array("h", pcm)
    for i in range(len(pcm)):
        v = pcm[i] + (0.22 * pcm[i - d1] if i >= d1 else 0) + (0.14 * pcm[i - d2] if i >= d2 else 0)
        out[i] = int(max(-32768, min(32767, v / 1.2)))
    return out


def mulaw(s):
    """G.711 mu-law encode one 16-bit sample."""
    BIAS, CLIP = 0x84, 32635
    sign = 0x80 if s < 0 else 0
    if s < 0:
        s = -s
    s = min(s, CLIP) + BIAS
    exp = 7
    mask = 0x4000
    while exp > 0 and not (s & mask):
        exp -= 1
        mask >>= 1
    mant = (s >> (exp + 3)) & 0x0F
    return ~(sign | (exp << 4) | mant) & 0xFF


def build(voice, rate, out, style):
    clips = []
    with tempfile.TemporaryDirectory() as td:
        for i, (name, text) in enumerate(PHRASES.items()):
            assert len(name) < 20, name
            pcm = render(text, voice, rate, os.path.join(td, "c.wav"))
            if style == "jarvis":
                pcm = style_jarvis(pcm)
            pcm = trim_and_normalize(pcm)
            clips.append((name, bytes(mulaw(s) for s in pcm)))
            print(f"\r{i + 1}/{len(PHRASES)} {name:<20}", end="", flush=True)
    print()
    count = len(clips)
    hdr = struct.pack("<4sHHI24s", b"CMVP", 1, count, RATE, voice.encode()[:23])
    off = len(hdr) + 28 * count
    index, data = b"", b""
    for name, mu in clips:
        index += struct.pack("<20sII", name.encode(), off + len(data), len(mu))
        data += mu
    blob = hdr + index + data
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(blob)
    secs = len(data) / RATE
    print(f"wrote {out}: {count} clips, {secs:.1f} s of speech, {len(blob) / 1024:.0f} KB "
          f"(voice partition holds 3072 KB)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--voice", default="Daniel", help="macOS voice (say -v '?' lists them)")
    ap.add_argument("--rate", type=int, default=180, help="words per minute")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "build", "voice.bin"))
    ap.add_argument("--style", choices=["jarvis", "plain"], default="jarvis",
                    help="voice processing (default: jarvis)")
    ap.add_argument("--audition", metavar="TEXT", help="play TEXT on the Mac exactly as the meter would")
    a = ap.parse_args()
    if a.audition:
        with tempfile.TemporaryDirectory() as td:
            src, dst = os.path.join(td, "a.wav"), os.path.join(td, "b.wav")
            pcm = render(a.audition, a.voice, a.rate, src)
            if a.style == "jarvis":
                pcm = style_jarvis(pcm)
            with wave.open(dst, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
                w.writeframes(pcm.tobytes())
            subprocess.run(["afplay", dst], check=True)
        return
    build(a.voice, a.rate, a.out, a.style)


if __name__ == "__main__":
    sys.exit(main())
