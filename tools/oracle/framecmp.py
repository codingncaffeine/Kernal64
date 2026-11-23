#!/usr/bin/env python3
"""Compares a raster from k64run --raster with a reference screenshot taken with
tools/oracle/vice.sh (-VICIIborders 2). The reference emulator's RGB for each color
index is read from tools/oracle/vice-colors-MODEL.txt (measured with calib.py).

    framecmp.py RASTER.bin REF.png [--dx N --dy N] [--mask X0,Y0,X1,Y1]... [--diff OUT.png]

The reference image covers the whole raster; its first column and row are aligned with
ours by DX/DY (our column = (ref column + DX) mod width, our line = (ref row + DY) mod
height). Without --dx/--dy the alignment with the fewest differing pixels is searched and
printed. --mask excludes a rectangle of reference pixels (inclusive). Exit status 0 when
the compared pixels are identical.
"""
import argparse, sys
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument('raster'); ap.add_argument('ref')
ap.add_argument('--dx', type=int); ap.add_argument('--dy', type=int)
ap.add_argument('--mask', action='append', default=[])
ap.add_argument('--diff')
ap.add_argument('--colors', default='vice-colors-pal.txt', help='color table in tools/oracle/')
a = ap.parse_args()

img = Image.open(a.ref).convert('RGB')
W, H = img.size
raw = open(a.raster, 'rb').read()
if len(raw) != W * H:
    sys.exit(f'framecmp: raster is {len(raw)} bytes, reference is {W}x{H}')
px = img.load()
import os
colors = {}
for n, l in enumerate(l for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), a.colors)) if not l.startswith('#')):
    v = int(l.strip(), 16)
    colors[(v >> 16, (v >> 8) & 255, v & 255)] = n
assert len(colors) == 16
ref = [[0] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        c = px[x, y]
        if c not in colors:
            sys.exit(f'framecmp: reference pixel {x},{y} = {c[0]:02x}{c[1]:02x}{c[2]:02x} is not a known color')
        ref[y][x] = colors[c]
masks = [tuple(int(v) for v in m.split(',')) for m in a.mask]
def masked(x, y):
    return any(x0 <= x <= x1 and y0 <= y <= y1 for x0, y0, x1, y1 in masks)

def count(dx, dy, limit=None):
    n = 0
    for y in range(H):
        row = raw[((y + dy) % H) * W:((y + dy) % H + 1) * W]
        ry = ref[y]
        for x in range(W):
            if row[(x + dx) % W] != ry[x] and not masked(x, y):
                n += 1
                if limit is not None and n > limit:
                    return n
    return n

if a.dx is None or a.dy is None:
    best = None
    # Rows first on a coarse column grid, then columns.
    for dy in range(H):
        for dx in range(0, W, 8):
            n = count(dx, dy, best[0] if best else None)
            if best is None or n < best[0]:
                best = (n, dx, dy)
    n0, dx0, dy0 = best
    for dx in range(dx0 - 8, dx0 + 9):
        n = count(dx % W, dy0, best[0])
        if n < best[0]:
            best = (n, dx % W, dy0)
    print(f'framecmp: best alignment dx={best[1]} dy={best[2]} ({best[0]} pixels differ)')
    dx, dy = best[1], best[2]
else:
    dx, dy = a.dx, a.dy

diffs = []
for y in range(H):
    for x in range(W):
        ours = raw[((y + dy) % H) * W + (x + dx) % W]
        if ours != ref[y][x] and not masked(x, y):
            diffs.append((x, y, ours, ref[y][x]))
print(f'framecmp: {W}x{H}, dx={dx} dy={dy}, {len(diffs)} pixels differ')
for d in diffs[:12]:
    print(f'  ref x={d[0]} y={d[1]}: ours {d[2]}, reference {d[3]}')
if a.diff:
    out = Image.new('RGB', (W, H))
    o = out.load()
    for y in range(H):
        for x in range(W):
            v = ref[y][x] * 12
            o[x, y] = (v, v, v)
    for x, y, _, _ in diffs:
        o[x, y] = (255, 0, 0)
    out.save(a.diff)
sys.exit(1 if diffs else 0)
