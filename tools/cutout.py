#!/usr/bin/env python3
"""Cuts artwork out of a flat background into a transparent PNG.

    tools/cutout.py IN OUT [--pad N] [--max-width W]

The background color is read from the image border. Only background that is
connected to the border is removed, so the same color inside the artwork stays.
Edge pixels get partial alpha, with the background color unmixed from them so no
colored fringe is left on other backgrounds. Anything not connected to the
largest piece of artwork (stray marks beside it) is dropped, then the result is
trimmed to the artwork plus padding.
"""
import argparse
import sys

import numpy as np
from PIL import Image
from collections import deque


def label(mask: np.ndarray):
    """4-connected regions of a boolean image: (labels, count)."""
    h, w = mask.shape
    labels = np.zeros((h, w), dtype=np.int32)
    flat_mask = mask.ravel()
    flat = labels.ravel()
    n = 0
    for start in np.flatnonzero(flat_mask):
        if flat[start]:
            continue
        n += 1
        flat[start] = n
        q = deque([start])
        while q:
            i = q.popleft()
            y, x = divmod(i, w)
            for j, ok in ((i - w, y > 0), (i + w, y < h - 1), (i - 1, x > 0), (i + 1, x < w - 1)):
                if ok and flat_mask[j] and not flat[j]:
                    flat[j] = n
                    q.append(j)
    return labels, n


def fill_holes(mask: np.ndarray) -> np.ndarray:
    """The mask plus every region of not-mask that does not touch the border."""
    outside, _ = label(~mask)
    edge = np.unique(np.concatenate([outside[0], outside[-1], outside[:, 0], outside[:, -1]]))
    return mask | ~np.isin(outside, edge[edge != 0])


def dilate(mask: np.ndarray, iterations: int) -> np.ndarray:
    m = mask.copy()
    for _ in range(iterations):
        g = m.copy()
        g[1:] |= m[:-1]; g[:-1] |= m[1:]; g[:, 1:] |= m[:, :-1]; g[:, :-1] |= m[:, 1:]
        m = g
    return m


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--pad", type=int, default=8)
    ap.add_argument("--max-width", type=int, default=0)
    ap.add_argument("--inner", type=float, default=22.0, help="distance below which a pixel is background")
    ap.add_argument("--outer", type=float, default=70.0, help="distance above which a pixel is artwork")
    a = ap.parse_args()

    img = np.asarray(Image.open(a.src).convert("RGB")).astype(np.float64)
    h, w, _ = img.shape
    border = np.concatenate([img[:4].reshape(-1, 3), img[-4:].reshape(-1, 3),
                             img[:, :4].reshape(-1, 3), img[:, -4:].reshape(-1, 3)])
    bg = np.median(border, axis=0)
    dist = np.sqrt(((img - bg) ** 2).sum(axis=2))

    # Background = pixels near the background color that connect to the border.
    near = dist < a.outer
    labels, _ = label(near)
    edge_labels = np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))
    edge_labels = edge_labels[edge_labels != 0]
    outside = np.isin(labels, edge_labels)

    # Alpha: 0 well inside the background, 1 in the artwork, a ramp across the edge.
    ramp = np.clip((dist - a.inner) / (a.outer - a.inner), 0.0, 1.0)
    alpha = np.where(outside, ramp, 1.0)

    # Keep only the largest piece of artwork (with the holes inside it).
    solid = alpha > 0.5
    pieces, n = label(solid)
    if n == 0:
        print("cutout: nothing left after removing the background", file=sys.stderr)
        return 1
    sizes = np.bincount(pieces.ravel(), minlength=n + 1)[1:]
    main_piece = fill_holes(pieces == (int(np.argmax(sizes)) + 1))
    keep = dilate(main_piece, 3)
    alpha = np.where(keep, alpha, 0.0)

    # Unmix the background from partly transparent pixels: c = a*f + (1-a)*bg.
    safe = np.maximum(alpha, 1e-3)[..., None]
    fg = np.clip((img - (1.0 - alpha[..., None]) * bg) / safe, 0, 255)
    fg = np.where(alpha[..., None] >= 0.999, img, fg)

    ys, xs = np.nonzero(alpha > 0.02)
    y0, y1 = max(ys.min() - a.pad, 0), min(ys.max() + a.pad + 1, h)
    x0, x1 = max(xs.min() - a.pad, 0), min(xs.max() + a.pad + 1, w)
    rgba = np.dstack([fg, alpha * 255.0])[y0:y1, x0:x1]
    out = Image.fromarray(np.round(rgba).astype(np.uint8), "RGBA")
    if a.max_width and out.width > a.max_width:
        out = out.resize((a.max_width, round(out.height * a.max_width / out.width)), Image.LANCZOS)
    out.save(a.dst, optimize=True)
    print(f"cutout: {a.dst} {out.width}x{out.height} (background {tuple(int(c) for c in bg)}, {n} pieces, kept 1)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
