"""The Button Mapping drum's arrows (K, Nov 14: the style hints' arrows, which match the text, instead of the font's
thin ones): the game's own arrow shape (system texture 20's "+down" piece, button_prompt_sheets.cpp: a shaft 4 of 14
wide down to 12 of 19, a head 14 wide to the point at 19) in the drum text's two looks, its fill and its 1-line stroke.

python3 tools/options/make_drum_arrows.py [OUT_DIR]   (default resources/options/mapping)
writes arrow_down.png, arrow_up.png (cream DED3B9, black stroke) and arrow_down_lit.png, arrow_up_lit.png (the hovered
item: cream FFF8CE, red-brown A12E02 stroke). The core is CAP lines tall (the drum's cap); the picture has MARGIN lines
round it (options_overlay.cpp kMapArrow*). Needs numpy, scipy, Pillow.
"""
import os, sys
import numpy as np
from scipy import ndimage as nd
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CAP = 15.5        # options_overlay.cpp kMapCap: the core's height, lines
STROKE = 1.4      # the text's outline (kStroke 1.0, drawn as offset copies: it reads thicker), lines
MARGIN = 2.0      # round the core: the stroke and its soft edge
SOFT = 0.69       # kBlur: the text's softening, lines
PX = 8            # pixels per line
LOOKS = {'': ((0xDE, 0xD3, 0xB9), (0x01, 0x01, 0x00)), '_lit': ((0xFF, 0xF8, 0xCE), (0xA1, 0x2E, 0x02))}


def core_distance(x, y):
    """Signed-ish distance (0 inside) to the down arrow in core units (x across 0..14, y along 0..19)."""
    sx = np.maximum.reduce([5.0 - x, np.zeros_like(x), x - 9.0])
    sy = np.maximum.reduce([0.0 - y, np.zeros_like(y), y - 12.0])
    shaft = np.hypot(sx, sy)
    ax, ay = [0.0, 14.0, 7.0], [12.0, 12.0, 19.0]
    inside = np.ones_like(x, bool)
    edge = np.full_like(x, 1e9)
    for i in range(3):
        j = (i + 1) % 3
        ex, ey = ax[j] - ax[i], ay[j] - ay[i]
        cross = ex * (y - ay[i]) - ey * (x - ax[i])
        inside &= cross >= 0
        t = np.clip(((x - ax[i]) * ex + (y - ay[i]) * ey) / (ex * ex + ey * ey), 0, 1)
        edge = np.minimum(edge, np.hypot(x - (ax[i] + t * ex), y - (ay[i] + t * ey)))
    return np.minimum(shaft, np.where(inside, 0.0, edge))


def arrow(up, fill, stroke):
    unit = CAP / 19.0                                   # lines per core unit
    w_lines, h_lines = 14 * unit + 2 * MARGIN, CAP + 2 * MARGIN
    W, H = int(round(w_lines * PX)), int(round(h_lines * PX))
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32) + 0.5
    cx = (xs / PX - MARGIN) / unit
    cy = (ys / PX - MARGIN) / unit
    if up:
        cy = 19.0 - cy
    # Signed distance in lines (negative inside): the core's own distance outside, the distance to its edge inside.
    outside = core_distance(cx, cy) * unit
    inside_mask = outside <= 0.0
    depth = nd.distance_transform_edt(inside_mask) / PX
    d = np.where(inside_mask, 0.5 / PX - depth, outside)
    soft = SOFT
    a_fill = np.clip(0.5 - d / soft, 0, 1)
    a_all = np.clip(0.5 - (d - STROKE) / soft, 0, 1)
    rgb = np.array(fill, np.float32) * a_fill[..., None] + np.array(stroke, np.float32) * (1 - a_fill)[..., None]
    out = np.dstack([rgb, a_all * 255]).clip(0, 255).astype(np.uint8)
    return Image.fromarray(out, 'RGBA')


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'resources/options/mapping')
    for suffix, (fill, stroke) in LOOKS.items():
        for up in (False, True):
            im = arrow(up, fill, stroke)
            path = os.path.join(out, 'arrow_%s%s.png' % ('up' if up else 'down', suffix))
            im.save(path)
            print(path, im.size)


if __name__ == '__main__':
    main()
