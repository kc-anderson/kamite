"""Draws the OPTIONS pages' scroll bar at high resolution, in the look of the game's own Techniques list scroll bar
(an 8-texel-wide bar: a filled cream-to-tan capsule thumb with orange-red rims at its rounded ends, over an outlined
brown capsule track). Our own drawing from measured colours, so nothing of the game's art ships.

    python tools/options/make_scrollbar.py        -> resources/options/scroll_thumb.png, scroll_track.png

Each image is 8 x 16 texels (16 px a texel): the top and bottom quarters are the rounded ends, the middle stretches
(options_overlay.cpp draws them as image items with size 0.25).
"""
import os
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'resources/options')
S = 16             # pixels a texel
W, H = 8, 16       # texels
RADIUS = 2.6       # the ends' corner rounding, texels
SS = 4             # supersampling


def hexcol(c):
    return np.array([int(c[i:i + 2], 16) for i in (0, 2, 4)], float)


def capsule_distance(x, y, inset=0.0):
    """Signed distance (texels, negative inside) to a W x H rounded rectangle shrunk by inset."""
    r = max(RADIUS - inset, 0.01)
    hx, hy = W / 2 - inset, H / 2 - inset
    qx = np.abs(x - W / 2) - (hx - r)
    qy = np.abs(y - H / 2) - (hy - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def grid():
    n = W * S * SS
    m = H * S * SS
    x = (np.arange(n) + 0.5) / (S * SS)
    y = (np.arange(m) + 0.5) / (S * SS)
    return np.meshgrid(x, y)


def downsample(rgba):
    h, w, _ = rgba.shape
    a = rgba[..., 3:4]
    rgb = (rgba[..., :3] * a).reshape(h // SS, SS, w // SS, SS, 3).sum((1, 3))
    alpha = a.reshape(h // SS, SS, w // SS, SS, 1).sum((1, 3))
    out = np.concatenate([np.where(alpha > 0, rgb / np.maximum(alpha, 1e-6), 0), alpha / (SS * SS)], -1)
    return Image.fromarray(np.clip(out * 255, 0, 255).round().astype(np.uint8), 'RGBA')


def thumb():
    x, y = grid()
    # The body's columns, left to right (light just left of the middle, a darker tan edge on the right).
    cols = [hexcol(c) for c in ('ffdc8f', 'fff1aa', 'fff1aa', 'ffef95', 'ffdc8f', 'ffcc63', 'f8b360', 'dba553')]
    t = np.clip(x - 0.5, 0, W - 1)
    i = np.minimum(t.astype(int), W - 2)
    f = (t - i)[..., None]
    rgb = np.take(np.array(cols), i, 0) * (1 - f) + np.take(np.array(cols), i + 1, 0) * f
    d = capsule_distance(x, y)
    # Orange-red rims on the rounded ends only (where the edge curves), fading inward over about a texel.
    end = np.clip((RADIUS - np.minimum(y, H - y)) / RADIUS, 0, 1)
    curve = np.clip((np.abs(x - W / 2) - (W / 2 - RADIUS)) / RADIUS, 0, 1)
    rim = np.clip(1 + d / 0.9, 0, 1) * np.clip(end * (0.35 + 1.5 * curve), 0, 1) * 0.85
    rgb = rgb * (1 - rim[..., None]) + hexcol('f55a1c') * rim[..., None]
    alpha = np.clip(0.5 - d * S, 0, 1)  # about a pixel of anti-aliasing at the final size
    return downsample(np.dstack([rgb / 255, alpha]))


def track():
    x, y = grid()
    outer = capsule_distance(x, y)
    inner = capsule_distance(x, y, 2.0)
    # Rails two texels wide: the left one lighter (a tan outside, a brighter ochre inside), the right one darker.
    left_o, left_i, right_i, right_o = (hexcol(c) for c in ('a57241', 'b3882f', '7a542c', '7b5b35'))
    side = np.clip(x / W, 0, 1)[..., None]
    depth = np.clip(-outer / 2.0, 0, 1)[..., None]  # 0 at the outside of the rail, 1 at its inside
    rgb = (left_o * (1 - depth) + left_i * depth) * (1 - side) + (right_o * (1 - depth) + right_i * depth) * side
    px = 1.0 / S
    alpha = np.clip(0.5 - outer / px, 0, 1) * np.clip(0.5 + inner / px, 0, 1)
    return downsample(np.dstack([rgb / 255, alpha]))


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    thumb().save(os.path.join(OUT, 'scroll_thumb.png'))
    track().save(os.path.join(OUT, 'scroll_track.png'))
    print('scroll_thumb.png, scroll_track.png:', W * S, 'x', H * S)
