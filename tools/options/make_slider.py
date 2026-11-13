"""Draws the OPTIONS pages' slider fill in the look of the game's enemy health bar (a slate-blue-to-white gradient
with a darker bottom edge, K's pick, Nov 13). Our own drawing from colours measured on the disc's gauge texture, so
nothing of the game's art ships.

    python tools/options/make_slider.py        -> resources/options/slider_fill.png

The image is stretched over the filled part (the whole gradient always shows, as the enemy bar's does at any health);
options_overlay.cpp draws the dark background behind it.
"""
import os
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'resources/options/slider_fill.png')
W, H, SHADE = 512, 40, 8  # pixels; the bottom SHADE rows are the darker edge (a fifth, as on the gauge)

# Left to right, evenly spaced: the body and the bottom edge.
BODY = ['6b82a0', '6c83a1', '7188a6', '8096b2', '98a9c1', 'b8c6db', 'dee5eb', 'f2f4f6', 'fdfdfd']
EDGE = ['576e88', '5b738e', '697c96', '7886a3', '8999af', '9caabc', 'abb7c7', 'b7c5d0', 'c1c7d2']


def ramp(stops):
    cols = np.array([[int(c[i:i + 2], 16) for i in (0, 2, 4)] for c in stops], float)
    t = np.linspace(0, len(stops) - 1, W)
    i = np.minimum(t.astype(int), len(stops) - 2)
    f = (t - i)[:, None]
    return cols[i] * (1 - f) + cols[i + 1] * f


def main():
    rgb = np.empty((H, W, 3))
    rgb[:H - SHADE] = ramp(BODY)
    rgb[H - SHADE:] = ramp(EDGE)
    alpha = np.full((H, W, 1), 255.0)
    Image.fromarray(np.concatenate([rgb, alpha], -1).round().astype(np.uint8), 'RGBA').save(OUT)
    print(OUT)


if __name__ == '__main__':
    main()
