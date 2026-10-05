#!/usr/bin/env python3
"""R2-D2 pixel art for the AiPi's splash, in claudepix shape (so gen_clawd.py
packs it with Clawd's animations and clawd.c plays it unchanged).

    python3 tools/r2_pix.py [OUT.png]     # preview every frame (needs Pillow)

Edit the art here, then re-run tools/gen_clawd.py.
"""
import sys

# char -> palette index; index 0 is transparent
KEY = ".#:bkrcg"
PALETTE = ["transparent",
           "#E8ECF2",   # 1 # white body
           "#9AA4B4",   # 2 : silver / shading
           "#2B6CE8",   # 3 b R2 blue
           "#0E1116",   # 4 k black
           "#FF3B3B",   # 5 r red logic light
           "#9CCBFF",   # 6 c lens glint / blue logic light
           "#5B6472"]   # 7 g dark grey

# The dome band (rows 4-6, cols 5-14) is a strip around the dome; rotating it
# turns R2's head. The strip is wider than the window so a turn shows the back.
DOME_STRIP = [
    "#bbbb#bb####bb##",
    "#bkcb#bb#r##bb##",
    "#bkkb#######bb##",
]

TOP = [
    "....................",
    ".......::::::.......",
    "......:######:......",
    ".....:########:.....",
]
# body rows: left leg (cols 0-4) | body (cols 5-14) | right leg (cols 15-19)
_TORSO = [  # (left leg, body); the right leg mirrors the left
    ("..bbb", "##########"),
    ("..b:b", "#bb#kk#bb#"),
    ("..bbb", "#bb#bb#bb#"),
    ("...#b", "##########"),
    ("...#b", "#gggggggg#"),
    ("...#b", "##b#bb#b##"),
    ("...#b", "##b#bb#b##"),
    ("...#b", "##b####b##"),
    ("...#b", ".########."),
    ("...#b", "...::::..."),
]
BODY = ([".....::::::::::....."]
        + [leg + body + leg[::-1] for leg, body in _TORSO]
        + ["..::::..::::..::::..",
           "..gggg..gggg..gggg.."])


def frame(rot=0, logic="r", glint="c"):
    rows = list(TOP)
    for s in DOME_STRIP:
        win = (s[rot:] + s[:rot])[:10]
        win = win.replace("r", logic).replace("c", glint)
        rows.append("....." + win + ".....")
    rows += BODY
    assert len(rows) == 20 and all(len(r) == 20 for r in rows), rows
    return [[KEY.index(ch) for ch in r] for r in rows]


def anim(name, frames):
    return {"name": name, "palette": PALETTE,
            "frames": [{"hold": h, "grid": g} for g, h in frames]}


ANIMS = {
    # idle: logic light flips red <-> blue, the lens glints now and then
    "R2_IDLE": anim("r2 idle", [
        (frame(0, "r", "c"), 1400),
        (frame(0, "c", "c"), 260),
        (frame(0, "r", "c"), 900),
        (frame(0, "r", "k"), 120),
        (frame(0, "r", "c"), 1100),
        (frame(0, "c", "c"), 200),
        (frame(0, "r", "c"), 200),
        (frame(0, "c", "c"), 200),
    ]),
    # think (fetching): the dome swivels right and back, logic light chatters
    "R2_THINK": anim("r2 think", [
        (frame(0, "r"), 500),
        (frame(1, "c"), 110),
        (frame(2, "r"), 110),
        (frame(3, "c"), 110),
        (frame(4, "r"), 450),
        (frame(3, "c"), 110),
        (frame(2, "r"), 110),
        (frame(1, "c"), 110),
        (frame(0, "r"), 500),
        (frame(15, "c"), 130),
        (frame(14, "r"), 400),
        (frame(15, "c"), 130),
    ]),
}


def preview(path):
    from PIL import Image
    S = 8
    frames = [(n, f["grid"]) for n, a in ANIMS.items() for f in a["frames"]]
    img = Image.new("RGB", (len(frames) * (20 * S + 8), 20 * S), (0, 0, 0))
    pal = [(0, 0, 0)] + [tuple(int(p[i:i + 2], 16) for i in (1, 3, 5)) for p in PALETTE[1:]]
    for k, (_, g) in enumerate(frames):
        for y, row in enumerate(g):
            for x, v in enumerate(row):
                for dy in range(S):
                    for dx in range(S):
                        img.putpixel((k * (20 * S + 8) + x * S + dx, y * S + dy), pal[v])
    img.save(path)
    print(f"wrote {path}: {len(frames)} frames")


if __name__ == "__main__":
    preview(sys.argv[1] if len(sys.argv) > 1 else "r2_preview.png")
