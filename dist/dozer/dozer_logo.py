# SPDX-FileCopyrightText: Copyright 2026 Dozer contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Dozer's mark: a D whose flat edge is a bulldozer blade, with speed lines, on a dark tile.

The source of dist/dozer.ico, dist/dozer.icns (the macOS app icon), dist/dozer.bmp and
dist/qt_themes/default/icons/256x256/dozer.png;
needs only Pillow:
    python dist/dozer/dozer_logo.py [output folder for a preview sheet]
Drawn in a unit square at 8x and scaled down, so the edges stay smooth. 16 and 24 px leave out the
speed lines (they vanish that small) and fill the tile with the D.
"""

import math
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.dirname(HERE)

TILE_TOP = (52, 54, 60)
TILE_BOTTOM = (22, 23, 26)
YELLOW = (255, 196, 0)
YELLOW_DIM = (176, 134, 0)

# The D: flat left edge at x 0.30, round on the right (centre 0.44, 0.50, radius 0.30), with the
# counter cut out of it; three blade teeth on the flat edge.
D_LEFT, D_TOP, D_BOTTOM, D_ARC_START = 0.30, 0.20, 0.80, 0.44
COUNTER_LEFT, COUNTER_TOP, COUNTER_BOTTOM, COUNTER_ARC_START = 0.42, 0.34, 0.66, 0.46
TEETH_Y = (0.30, 0.50, 0.70)
TEETH_HALF_HEIGHT = 0.055
TEETH_DEPTH = 0.11
SPEED_LINES_Y = (0.38, 0.50, 0.62)


def arc(cx, cy, radius, start, end, steps=48):
    return [(cx + radius * math.cos(math.radians(start + (end - start) * i / steps)),
             cy + radius * math.sin(math.radians(start + (end - start) * i / steps)))
            for i in range(steps + 1)]


def d_polygon(left, top, bottom, arc_start):
    mid = (top + bottom) / 2
    radius = (bottom - top) / 2
    return [(left, top), (arc_start, top)] + arc(arc_start, mid, radius, -90, 90) + \
        [(arc_start, bottom), (left, bottom)]


def gradient(size, top, bottom):
    """A vertical gradient image (RGB) from top to bottom."""
    column = Image.new('RGB', (1, size))
    for y in range(size):
        t = y / max(1, size - 1)
        column.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return column.resize((size, size))


def render(size, simple=None, tile=True):
    simple = size <= 24 if simple is None else simple
    scale = 8
    big = size * scale
    # The small sizes centre the D and make it larger: the speed lines are gone.
    zoom, shift = (1.14, 0.035) if simple else (1.0, 0.0)

    def point(p):
        return ((0.5 + (p[0] + shift - 0.5) * zoom) * big, (0.5 + (p[1] - 0.5) * zoom) * big)

    image = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    if tile:
        mask = Image.new('L', (big, big), 0)
        inset = 0 if size <= 24 else round(big * 0.02)
        ImageDraw.Draw(mask).rounded_rectangle((inset, inset, big - 1 - inset, big - 1 - inset),
                                               radius=round(big * 0.22), fill=255)
        image.paste(gradient(big, TILE_TOP, TILE_BOTTOM), (0, 0), mask)
    letter = Image.new('L', (big, big), 0)
    draw = ImageDraw.Draw(letter)
    draw.polygon([point(p) for p in d_polygon(D_LEFT, D_TOP, D_BOTTOM, D_ARC_START)], fill=255)
    for y in TEETH_Y:
        draw.polygon([point((D_LEFT, y - TEETH_HALF_HEIGHT)), point((D_LEFT, y + TEETH_HALF_HEIGHT)),
                      point((D_LEFT - TEETH_DEPTH, y))], fill=255)
    draw.polygon([point(p) for p in d_polygon(COUNTER_LEFT, COUNTER_TOP, COUNTER_BOTTOM,
                                              COUNTER_ARC_START)], fill=0)
    image.paste(Image.new('RGBA', (big, big), YELLOW), (0, 0), letter)
    if not simple:
        lines = Image.new('L', (big, big), 0)
        draw = ImageDraw.Draw(lines)
        for i, y in enumerate(SPEED_LINES_Y):
            draw.rounded_rectangle((big * 0.80, big * (y - 0.012), big * (0.90 - i * 0.02),
                                    big * (y + 0.012)), radius=big * 0.012, fill=255)
        image.paste(Image.new('RGBA', (big, big), YELLOW_DIM), (0, 0), lines)
    return image.resize((size, size), Image.LANCZOS)


def write_bmp(image, path):
    """32-bit BI_RGB bottom-up BMP with alpha in the fourth byte, like dist/eden.bmp (the CLI's
    SDL window icon, compiled in by yuzu_cmd's CMakeLists)."""
    import struct
    w, h = image.size
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = image.getpixel((x, y))
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    pixels = b''.join(rows)
    header = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(header + info + pixels)


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [render(s) for s in sizes]
    ico = os.path.join(DIST, 'dozer.ico')
    frames[-1].save(ico, format='ICO', sizes=[(s, s) for s in sizes], append_images=frames[:-1])
    png = os.path.join(DIST, 'qt_themes', 'default', 'icons', '256x256', 'dozer.png')
    render(256).save(png, optimize=True)
    bmp = os.path.join(DIST, 'dozer.bmp')
    write_bmp(frames[-1], bmp)
    icns_frames = [render(s) for s in (16, 32, 64, 128, 256, 512)]
    icns = os.path.join(DIST, 'dozer.icns')
    icns_frames[-1].save(icns, format='ICNS', append_images=icns_frames[:-1])
    print(icns)
    print(ico)
    print(png)
    print(bmp)
    if len(sys.argv) > 1:  # preview: every size at 1x and 4x, on light and dark
        sheet = Image.new('RGBA', (1500, 620), (240, 240, 240, 255))
        sheet.paste((32, 32, 36, 255), (0, 310, 1500, 620))
        x = 10
        for s, f in zip(sizes, frames):
            for y0 in (10, 320):
                sheet.alpha_composite(f, (x, y0))
                if s <= 64:
                    sheet.alpha_composite(f.resize((s * 4, s * 4), Image.NEAREST), (x, y0 + 270 - s * 4))
            x += max(s, s * 4 if s <= 64 else s) + 12
        out = os.path.join(sys.argv[1], 'dozer_preview.png')
        sheet.save(out)
        print(out)


if __name__ == '__main__':
    main()
