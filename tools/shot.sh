#!/bin/zsh
# Grab what the meter's screen shows right now -> PNG (3x).  tools/shot.sh [out.png] [host]
out=${1:-${TMPDIR:-/tmp}/meter.png}; host=${2:-claude-meter.local}
curl -sf -m 15 -o "$out.bmp" "http://$host/api/screen.bmp" && sips -s format png "$out.bmp" --out "$out" >/dev/null && rm "$out.bmp" && echo "$out"
