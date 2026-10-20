#!/usr/bin/env python3
"""
preview.py - draws a screen dumped by the PC tests (tests/screen_*.bin) the
way the PS1 shows it: 80x30 cells of 8x8 pixels, same font and colours.
Lines are doubled, like on a TV (640x240 is shown with tall pixels).

    python3 tools/preview.py tests/screen_edit.bin preview.png
"""
import re
import struct
import sys
from pathlib import Path

from PIL import Image

COLS, ROWS = 80, 30
BG = (16, 24, 48)
# fg, bg (None = screen background); same as ps1/main.c
COLOURS = [
    ((220, 220, 210), None),             # A_TEXT
    ((255, 210, 90), None),              # A_HEAD
    ((10, 20, 40), (150, 180, 200)),     # A_STATUS
    ((255, 255, 255), (170, 30, 30)),    # A_WARN
    ((230, 230, 230), (50, 60, 90)),     # A_DLG
    ((10, 20, 40), (230, 200, 100)),     # A_DLG_SEL
    ((255, 220, 120), (50, 60, 90)),     # A_DLG_TITLE
]


def load_font():
    src = (Path(__file__).parent.parent / "ps1" / "font_latin1.h").read_text()
    rows = re.findall(r"\{ (0x[0-9A-F, x]+) \}", src)
    return [[int(v, 16) for v in r.split(",")] for r in rows]


def main():
    data = Path(sys.argv[1]).read_bytes()
    out = sys.argv[2] if len(sys.argv) > 2 else "preview.png"
    font = load_font()
    cx, cy, rows = struct.unpack_from("<hhh", data, COLS * ROWS * 2)
    img = Image.new("RGB", (COLS * 8, ROWS * 8), BG)
    top = (ROWS - rows) * 8 // 2               # centred, like on the PS1
    px = img.load()
    for y in range(rows):
        for x in range(COLS):
            ch, attr = data[(y * COLS + x) * 2], data[(y * COLS + x) * 2 + 1]
            fg, bg = COLOURS[attr]
            for r in range(8):
                bits = font[ch][r]
                for c in range(8):
                    on = bits >> c & 1
                    if x == cx and y == cy and r >= 6:
                        on = 1                     # underline cursor
                    if on:
                        px[x * 8 + c, top + y * 8 + r] = fg
                    elif bg:
                        px[x * 8 + c, top + y * 8 + r] = bg
    img = img.resize((COLS * 8, ROWS * 16), Image.NEAREST)
    img.save(out)


if __name__ == "__main__":
    main()
