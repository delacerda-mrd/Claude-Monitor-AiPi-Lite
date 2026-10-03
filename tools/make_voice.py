#!/usr/bin/env python3
"""Build the meter's voice pack with macOS text-to-speech.

    tools/make_voice.py [--voice Daniel] [--rate 180] [--style jarvis|plain] [--out build/voice.bin]
    tools/make_voice.py --audition "session_at #42 percent . resets_in ~9780"  # a device script, as the meter joins it
    tools/make_voice.py --say "Good evening."                                  # any text through the voice chain

Upload to a running meter (no USB needed):
    curl --data-binary @build/voice.bin http://claude-meter.local/voice

Needs numpy. Every clip is rendered by `say`, pitch-shifted
(--pitch, default a touch lighter), shaped by the voice chain (--style), loudness-matched, trimmed
with soft fades and IMA-ADPCM encoded (4 bits/sample at 16 kHz). The device
strings clips into sentences; see main/voice.c for the script format.

Why it sounds smooth (v2.3):
  * Fragments that lead into more speech ("Session at", "two hours") are
    rendered mid-sentence -- `say "Session at [[slnc 500]] and"`, cut at the
    silence -- so they keep a rising, continuing intonation instead of each
    word ending like a full stop.
  * Numbers are rendered fused with their unit ("forty two percent.",
    "three hours", "twelve minutes."), so the join that sounded worst is gone.
  * Clips keep their soft onsets (low trim threshold, 30 ms lead) and decay
    tails, and the device joins them with no added gap.

Pack layout (little-endian):
    "CMVP" u16 version=2  u16 count  u32 sample_rate  char voice[24]       (36 B)
    count x { char name[20]  u32 offset  u32 length }                      (28 B each)
    IMA-ADPCM data (offsets from the start of the pack; 2 samples per byte,
    low nibble first; each clip starts from predictor 0 / step index 0)
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import wave

import numpy as np

RATE = 16000          # the codec runs at 16 kHz (esp-sr's rate) since v2.2
VERSION = 2           # 1 = mu-law (v2.1/v2.2 packs), 2 = IMA-ADPCM
PART_KB = 0x5E0000 // 1024   # "voice" partition (partitions.csv, v2.4)

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


# name -> (spoken text, continues). Names are what device scripts reference
# (max 19 chars). continues=True: more speech follows, render mid-sentence.
PHRASES = {
    # building blocks
    "session_at":  ("Session at", True),
    "weekly_at":   ("Weekly at", True),
    "battery_at":  ("Battery at", True),
    "percent":     ("percent.", False),          # fallback when no p<N> clip
    "resets_in":   ("resets in", True),
    "back_in":     ("Back in", True),
    "and":         ("and", True),
    "heads_up":    ("Heads up.", False),
    "warning":     ("Warning.", False),
    "day":         ("day", True),
    "days":        ("days", True),
    "hour":        ("hour", True),
    "hours":       ("hours", True),
    "minute":      ("minute.", False),
    "minutes":     ("minutes.", False),
    # whole sentences
    "online":      ("Claude Meter online.", False),
    "token_new":   ("New token received.", False),
    "token_bad":   ("My token has expired. I've asked your Mac for a fresh one.", False),
    "offline":     ("Connection lost.", False),
    "back":        ("Back online.", False),
    "limit":       ("You've reached the limit.", False),
    "fresh":       ("Fresh window. Usage has reset.", False),
    "setup":       ("Setup mode. Scan the code on my screen to connect.", False),
    "ahead":       ("You're running ahead of pace.", False),
    "on_pace":     ("You're right on pace.", False),
    "room":        ("Plenty of room.", False),
    "test":        ("This is how I sound.", False),
    # voice control (v2.2)
    "yes":            ("Yes?", False),
    "sorry":          ("Sorry, I didn't catch that.", False),
    "refreshing":     ("Checking now.", False),
    "session_resets": ("Your session resets in", True),
    "weekly_resets":  ("Your week resets in", True),
    "quiet_on":       ("Quiet mode on.", False),
    "quiet_off":      ("Sound is back on.", False),
    "louder":         ("Louder.", False),
    "softer":         ("Quieter.", False),
    "night":          ("Goodnight.", False),
    "its":            ("It's", True),
    "oclock":         ("o'clock.", False),
    "oh":             ("oh", True),
    "am":             ("A M.", False),
    "pm":             ("P M.", False),
    "welcome":        ("You're welcome.", False),
    "intro":          ("I'm R1. I keep an eye on your Claude usage.", False),
    "charging_now":   ("and charging.", False),
}
# Personality (v2.4): device scripts say "@wake", "@ahead", ... and the meter
# picks one variant at random, never one it used recently (main/voice.c).
# Wake replies stay short: listening starts when the reply ends.
QUIPS = {
    "wake": ["Yes?", "Sir?", "Yes, sir?", "You rang?", "At your service.", "Listening.",
             "Go ahead.", "What is it now?", "I'm all ears, sir.", "Again?", "How can I help?"],
    "sorry": ["Sorry, I didn't catch that.", "I'm afraid that's not in my vocabulary, sir.",
              "Once more, sir? Perhaps with consonants.", "I didn't quite get that.",
              "Mumbling, sir. We've discussed this.", "That was either a command or a sneeze.",
              "Could you try that again? Slowly, this time."],
    "thanks": ["You're welcome.", "Always a pleasure, sir.", "Don't mention it.",
               "I live to serve. It's in the job description.", "Happy to help. Mostly.",
               "A thank you. How refreshing."],
    "hello": ["I'm R1. I keep an eye on your Claude usage.",
              "R1, at your service. Someone has to watch the token budget.",
              "Hello, sir. Still here. Still counting.",
              "I'm R1. Think of me as your conscience, with a battery.",
              "Hello again. People will say we're talking too much."],
    "ahead": ["You're running ahead of pace.", "Ahead of pace, sir. Perhaps ease off the throttle.",
              "At this rate, we'll be out of tokens by teatime.",
              "Ahead of pace. I'll pretend I didn't see that.",
              "That's a lot of thinking, even for you.",
              "You're burning through it, sir. Merely an observation."],
    "on_pace": ["You're right on pace.", "On pace. How uncharacteristically disciplined.",
                "Right on schedule. I'm almost impressed.", "On pace. Do keep it up, sir.",
                "Perfectly on pace. Someone's been reading the manual."],
    "room": ["Plenty of room.", "Plenty left in the tank, sir.",
             "Light usage. Are you feeling alright?", "Lots of room. Do try to use it wisely.",
             "Barely touched it. Day off, sir?"],
    "heads_up": ["Heads up.", "A gentle reminder, sir.", "Just so you're aware.",
                 "Friendly warning.", "Thought you'd like to know."],
    "warning": ["Warning.", "Sir, I must insist.", "This is your formal warning.",
                "Running rather hot, sir.", "I'd slow down, if I were you."],
    "limit": ["You've reached the limit.", "And that's the limit. I did warn you.",
              "We've hit the wall, sir.", "Limit reached. Perhaps a walk?",
              "That's all, sir. The well is dry."],
    "fresh": ["Fresh window. Usage has reset.", "Usage reset. A clean slate, sir.",
              "Fresh window. Try not to spend it all at once.",
              "New window. Let's be sensible this time, shall we?",
              "The meter has reset. Your reputation, less so."],
    "online": ["Claude Meter online.", "All systems online, sir.", "Online. Did you miss me?",
               "Good to be back. Shall we?", "Systems online. Let's see what you've been up to."],
    "offline": ["Connection lost.", "We've lost the network, sir.", "No network. I'm flying blind."],
    "back": ["Back online.", "And we're back.", "Network restored. Crisis averted."],
    "token_bad": ["My token has expired. I've asked your Mac for a fresh one.",
                  "My credentials have lapsed. Asking your Mac for new ones.",
                  "Token expired, sir. I've sent for a fresh one."],
    "token_new": ["New token received.", "Fresh credentials. Much obliged.",
                  "Token received. Back to work."],
    "night": ["Goodnight.", "Going dark.", "Lights out, sir.", "I'll be here."],
    "refresh": ["Checking now.", "One moment.", "Let me look.", "Consulting the numbers."],
    "mute": ["Quiet mode on.", "Fine. I'll keep my thoughts to myself.", "Silence it is.",
             "Muting. Under protest."],
    "unmute": ["Sound is back on.", "Ah. My voice returns.", "You missed me. Admit it."],
    "thinking": ["One moment.", "Let me think.", "Give me a second, sir.", "Consulting the Mac.",
                 "Thinking.", "Hmm. One moment."],
    "nobrain": ["My brain on the Mac isn't answering, sir.",
                "I can't reach the Mac. Is it asleep?",
                "The Mac isn't responding. I'm on my own, I'm afraid."],
    "louder": ["Louder.", "Is this better?", "Turning it up."],
    "softer": ["Quieter.", "Softer, then.", "Turning it down. No offence taken."],
    # unprompted, now and then, while you work (usage.c maybe_quip)
    "idle": ["Still here, sir. Still watching your tokens.",
             "Have you considered drinking some water? Just a thought.",
             "Do stand up occasionally, sir. Your spine sends its regards.",
             "I've run the numbers. You could use a break.",
             "Another day, another several thousand tokens.",
             "For the record, I'm doing a splendid job.",
             "Shall I order lunch? Ah. No hands.",
             "Do remember to blink, sir.",
             "You know, some people go outside.",
             "Status check. You're fine. I'm fine. Carry on.",
             "I'd make you a coffee, but, well. No arms.",
             "Another brilliant idea, I presume?",
             "I'm not saying it's an obsession. I'm just keeping count.",
             "If anyone asks, you're being very productive."],
    "idle_hot": ["You're burning through tokens rather quickly, sir.",
                 "Perhaps let Claude think a little less hard?",
                 "At this pace, I'll be announcing the limit soon. I'd rather not.",
                 "Might I suggest fewer retries, and more coffee?",
                 "The meter is getting rather warm, sir.",
                 "Steady on. There's still a lot of week left."],
    "idle_late": ["It's rather late, sir. Even I'd consider sleeping.",
                  "Shouldn't you be asleep?", "Late night again. I'll make a note of it.",
                  "The sensible thing would be bed. Just saying.",
                  "Burning the midnight oil, sir?"],
    "idle_batt": ["My battery's getting low, sir. A charger would be lovely.",
                  "Running on fumes here. Plug me in?",
                  "Low battery. I'd hate to miss anything."],
}
for _cat, _lines in QUIPS.items():
    for _i, _t in enumerate(_lines):
        PHRASES[f"q_{_cat}_{_i:02d}"] = (_t, False)

for _n in range(60):                                   # clock times, fallbacks
    PHRASES[str(_n)] = (number_words(_n), True)
for _n in range(101):                                  # "#N percent" -> p<N>
    PHRASES[f"p{_n}"] = (number_words(_n) + " percent.", False)
for _n in range(1, 8):                                 # durations (voice.c add_unit)
    PHRASES[f"d{_n}"] = (number_words(_n) + (" day" if _n == 1 else " days"), True)
for _n in range(1, 24):
    PHRASES[f"h{_n}"] = (number_words(_n) + (" hour" if _n == 1 else " hours"), True)
for _n in range(1, 60):
    PHRASES[f"m{_n}"] = (number_words(_n) + (" minute." if _n == 1 else " minutes."), False)


# ---------------------------------------------------------------- render
def render(text, voice, rate, pitch, cont, td):
    """`say` at RATE/pitch, played back at RATE: pitch < 1 is deeper and a
    touch slower. A continuing fragment is spoken mid-sentence and cut at
    the embedded silence."""
    src = int(round(RATE / pitch))
    path = os.path.join(td, "c.wav")
    spoken = f"{text} [[slnc 500]] and then" if cont else text
    subprocess.run(["say", "-v", voice, "-r", str(rate),
                    f"--data-format=LEI16@{src}", "-o", path, spoken], check=True)
    with wave.open(path) as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768
    if cont:
        x = x[:silence_cut(x, src)]
    return x


def silence_cut(x, sr, quiet=0.003, min_run=0.3):
    """Index where the first >= min_run s stretch of near-silence after the
    speech starts (the [[slnc]] gap)."""
    loud = np.abs(x) > quiet
    start = int(np.argmax(loud))
    run = 0
    for i in range(start, len(x)):
        run = 0 if loud[i] else run + 1
        if run >= min_run * sr:
            return i - run + 1
    return len(x)


# ---------------------------------------------------------------- voice chain
def biquad(x, b, a):
    y = np.zeros_like(x)
    b0, b1, b2 = b / a[0]
    a1, a2 = a[1] / a[0], a[2] / a[0]
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def highpass(x, f, q=0.707):
    w = 2 * np.pi * f / RATE
    al = np.sin(w) / (2 * q)
    c = np.cos(w)
    return biquad(x, np.array([(1 + c) / 2, -(1 + c), (1 + c) / 2]), np.array([1 + al, -2 * c, 1 - al]))


def peak_eq(x, f, gain_db, q):
    A = 10 ** (gain_db / 40)
    w = 2 * np.pi * f / RATE
    al = np.sin(w) / (2 * q)
    c = np.cos(w)
    return biquad(x, np.array([1 + al * A, -2 * c, 1 - al * A]), np.array([1 + al / A, -2 * c, 1 - al / A]))


def compress(x, thresh_db=-22, ratio=3.0, att=0.004, rel=0.09):
    """Feed-forward peak compressor: evens out word-to-word level so joined
    clips sound like one speaker in one breath."""
    ka, kr = np.exp(-1 / (att * RATE)), np.exp(-1 / (rel * RATE))
    env, out = 0.0, np.empty_like(x)
    t = 10 ** (thresh_db / 20)
    for i, v in enumerate(x):
        a = abs(v)
        env = ka * env + (1 - ka) * a if a > env else kr * env + (1 - kr) * a
        g = (t * (env / t) ** (1 / ratio)) / env if env > t else 1.0
        out[i] = v * g
    return out


def chain(x, style):
    if style == "plain":
        return x
    # "jarvis": clean, close and composed -- the tiny speaker can't do bass,
    # so cut the mud, lift diction, and hold the level steady.
    x = highpass(x, 130)
    x = peak_eq(x, 300, -2.5, 1.0)      # less boxy
    x = peak_eq(x, 3200, 3.5, 0.9)      # presence / crisp consonants
    return compress(x)


def finish(x, lead=0.030, tail=0.045, quiet_db=-46, target_rms_db=-17, ceiling=0.94):
    """Loudness-match on the voiced part, trim gently (soft onsets like the
    h in "hours" sit ~40 dB under the peak), fade the ends."""
    if not len(x) or not np.any(x):
        return np.zeros(0)
    loud = np.abs(x) > 10 ** (quiet_db / 20) * np.max(np.abs(x))
    a = max(int(np.argmax(loud)) - int(lead * RATE), 0)
    b = min(len(x) - int(np.argmax(loud[::-1])) + int(tail * RATE), len(x))
    x = x[a:b].copy()
    frames = x[: len(x) // 320 * 320].reshape(-1, 320)
    rms = np.sqrt((frames ** 2).mean(axis=1))
    voiced = rms[rms > rms.max() * 0.1]
    g = 10 ** (target_rms_db / 20) / (np.sqrt((voiced ** 2).mean()) or 1)
    g = min(g, ceiling / (np.max(np.abs(x)) or 1))
    x *= g
    fi, fo = int(0.004 * RATE), int(0.012 * RATE)
    x[:fi] *= np.linspace(0, 1, fi)
    x[-fo:] *= np.linspace(1, 0, fo)
    return x


def make_clip(text, cont, a, td):
    return finish(chain(render(text, a.voice, a.rate, a.pitch, cont, td), a.style))


# ---------------------------------------------------------------- IMA ADPCM
STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
         50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
         253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
         1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
         3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
         12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(pcm):
    """IMA ADPCM, low nibble first; mirrored by ima_step() in main/voice.c."""
    pred, idx, nib = 0, 0, []
    for s in pcm:
        step = STEPS[idx]
        d = int(s) - pred
        code = 8 if d < 0 else 0
        d = abs(d)
        diff = step >> 3
        if d >= step: code |= 4; d -= step; diff += step
        step >>= 1
        if d >= step: code |= 2; d -= step; diff += step
        step >>= 1
        if d >= step: code |= 1; diff += step
        pred = max(-32768, min(32767, pred - diff if code & 8 else pred + diff))
        idx = max(0, min(88, idx + IDX[code & 7]))
        nib.append(code)
    if len(nib) & 1:
        nib.append(0)
    return bytes(nib[i] | (nib[i + 1] << 4) for i in range(0, len(nib), 2))


def adpcm_decode(data):
    pred, idx, out = 0, 0, []
    for byte in data:
        for code in (byte & 15, byte >> 4):
            step = STEPS[idx]
            diff = step >> 3
            if code & 4: diff += step
            if code & 2: diff += step >> 1
            if code & 1: diff += step >> 2
            pred = max(-32768, min(32767, pred - diff if code & 8 else pred + diff))
            idx = max(0, min(88, idx + IDX[code & 7]))
            out.append(pred)
    return out


def to_i16(x):
    return np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16)


# ---------------------------------------------------------------- device mirror
GAP_MS, PAUSE_MS = 0, 240          # keep in step with main/voice.c


def expand(script):
    """Device script -> [(clip, pause_ms)], as main/voice.c does it."""
    toks, steps = script.split(), []

    def clip(n):
        steps.append([n, GAP_MS])

    def unit(v, pre, one, many):
        if f"{pre}{v}" in PHRASES: clip(f"{pre}{v}")
        else: clip(str(v)); clip(one if v == 1 else many)

    i = 0
    while i < len(toks):
        t = toks[i]
        if t.startswith("#"):
            v = max(0, min(100, int(t[1:])))
            if i + 1 < len(toks) and toks[i + 1] == "percent":
                clip(f"p{v}"); i += 1
            else:
                clip(str(v))
        elif t.startswith("~"):
            s = max(60, int(t[1:]))
            d, h, m = s // 86400, s % 86400 // 3600, s % 3600 // 60
            if d: unit(d, "d", "day", "days"); h and unit(h, "h", "hour", "hours")
            elif h: unit(h, "h", "hour", "hours"); m and unit(m, "m", "minute", "minutes")
            else: unit(m, "m", "minute", "minutes")
        elif t == ".":
            if steps: steps[-1][1] = PAUSE_MS
        else:
            clip(t)
        i += 1
    return steps


def play(x):
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "a.wav")
        with wave.open(p, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
            w.writeframes(to_i16(x).tobytes())
        subprocess.run(["afplay", p], check=True)


# ---------------------------------------------------------------- build
def build(a):
    clips = []
    with tempfile.TemporaryDirectory() as td:
        for i, (name, (text, cont)) in enumerate(PHRASES.items()):
            assert len(name) < 20, name
            clips.append((name, adpcm_encode(to_i16(make_clip(text, cont, a, td)))))
            print(f"\r{i + 1}/{len(PHRASES)} {name:<20}", end="", flush=True)
    print()
    count = len(clips)
    hdr = struct.pack("<4sHHI24s", b"CMVP", VERSION, count, RATE, a.voice.encode()[:23])
    off = len(hdr) + 28 * count
    index, data = b"", b""
    for name, enc in clips:
        index += struct.pack("<20sII", name.encode(), off + len(data), len(enc))
        data += enc
    blob = hdr + index + data
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(blob)
    secs = len(data) * 2 / RATE
    print(f"wrote {a.out}: {count} clips, {secs:.1f} s of speech, {len(blob) / 1024:.0f} KB "
          f"(voice partition holds {PART_KB} KB)")
    if len(blob) > PART_KB * 1024:
        sys.exit("pack too big for the voice partition")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--voice", default="Daniel", help="macOS voice (say -v '?' lists them)")
    ap.add_argument("--rate", type=int, default=176, help="words per minute before --pitch")
    ap.add_argument("--pitch", type=float, default=1.04,
                    help="playback pitch/speed factor (<1 deeper and slower, >1 lighter; default 1.04)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "build", "voice.bin"))
    ap.add_argument("--style", choices=["jarvis", "plain"], default="jarvis",
                    help="voice chain (default: jarvis)")
    ap.add_argument("--audition", metavar="SCRIPT",
                    help="play a device script (clip names, #N, ~S, .) joined exactly as the meter does")
    ap.add_argument("--say", metavar="TEXT", help="play TEXT through the voice chain")
    ap.add_argument("--wav", metavar="PATH", help="with --audition/--say: write a WAV instead of playing")
    a = ap.parse_args()
    if a.audition or a.say:
        with tempfile.TemporaryDirectory() as td:
            if a.say:
                x = make_clip(a.say, False, a, td)
            else:
                parts = []
                for name, pause in expand(a.audition):
                    text, cont = PHRASES[name]
                    pcm = adpcm_decode(adpcm_encode(to_i16(make_clip(text, cont, a, td))))
                    parts += [np.array(pcm) / 32768, np.zeros(RATE * pause // 1000)]
                x = np.concatenate(parts)
        if a.wav:
            with wave.open(a.wav, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
                w.writeframes(to_i16(x).tobytes())
        else:
            play(x)
        return
    build(a)


if __name__ == "__main__":
    sys.exit(main())
