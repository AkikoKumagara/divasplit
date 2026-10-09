#!/usr/bin/env python3
"""Draw the installer's bubble and LiveArea images (8-bit paletted PNGs, as the
Vita requires) with the Terminus font from src/font_ter_u14b.h."""
import os
import re

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})',
        open(os.path.join(HERE, '..', 'src', 'font_ter_u14b.h')).read().split('{', 1)[1])]
TEAL = (0x39, 0xC5, 0xBB)
DARK = (0x12, 0x18, 0x20)
WHITE = (0xF0, 0xF0, 0xF0)
GRAY = (0x90, 0x98, 0xA0)


def text(img, s, scale, color, cx=None, y=0, x=None):
    w = len(s) * 8 * scale
    x0 = (img.width - w) // 2 if x is None else x
    d = ImageDraw.Draw(img)
    for i, ch in enumerate(s):
        for row in range(14):
            bits = FONT[ord(ch) * 14 + row]
            for col in range(8):
                if bits >> (7 - col) & 1:
                    px = x0 + (i * 8 + col) * scale
                    py = y + row * scale
                    d.rectangle([px, py, px + scale - 1, py + scale - 1], fill=color)


def gradient(w, h, top, bottom):
    img = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    return img


def save(img, path):
    img.quantize(colors=256).save(os.path.join(HERE, 'sce_sys', path), optimize=True)


icon = gradient(128, 128, (0x1C, 0x2B, 0x33), DARK)
# the bubble is masked to a circle: keep the text well inside it
text(icon, 'diva', 2, WHITE, y=34)
text(icon, 'split', 2, TEAL, y=66)
save(icon, 'icon0.png')

bg = gradient(840, 500, (0x1C, 0x2B, 0x33), DARK)
text(bg, 'divasplit', 6, TEAL, y=150)
text(bg, 'installer', 3, WHITE, y=250)
text(bg, '60 fps + frame pacing for Project DIVA', 2, GRAY, y=330)
save(bg, 'livearea/contents/bg.png')

startup = gradient(280, 158, TEAL, (0x24, 0x8C, 0x85))
text(startup, 'divasplit', 3, WHITE, y=36)
text(startup, 'install / update', 2, DARK, y=96)
save(startup, 'livearea/contents/startup.png')
