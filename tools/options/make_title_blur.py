"""The OPTIONS titles' motion-blurred twins (title_<name>_blur.png), shown while L1 / R1 slide a category title in or out
(options_overlay.cpp titleSwitch). The game does the same on its title screen: each menu word (NEW GAME, LOAD GAME,
OPTIONS, the difficulties) has a horizontally streaked copy beside it in idtitlecore_us.idd that is shown while the word
moves. Measured there: the streak runs about 0.7 of the letters' height.

    python tools/options/make_title_blur.py [--dir resources/options]

For each category's title_<name>.png (make_splash.py): a quarter-size copy on a
canvas PAD of the title's width wider on each side (the streak spills past the letters), blurred along x only with a
box STREAK x the letters' height long (premultiplied, so the transparent edge does not darken).
"""
import argparse, os
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NAMES = ('controls', 'display', 'interface', 'gameplay', 'sound')  # OPTIONS never slides
PAD = 0.15     # of the title's width, each side (options_overlay.cpp kTitleBlurPad)
STREAK = 0.7   # of the letters' height
SHRINK = 4


def blur(path):
    im = np.asarray(Image.open(path).convert('RGBA')).astype(np.float64) / 255.0
    h, w = im.shape[:2]
    rows = np.where((im[..., 3] > 0.5).any(1))[0]
    letters = rows.max() - rows.min() + 1 if rows.size else h
    pad = int(round(PAD * w))
    canvas = np.zeros((h, w + 2 * pad, 4))
    canvas[:, pad:pad + w] = im
    canvas[..., :3] *= canvas[..., 3:4]  # premultiplied
    small = np.stack([np.asarray(Image.fromarray(canvas[..., c].astype(np.float32), 'F')
                                 .resize(((w + 2 * pad) // SHRINK, h // SHRINK), Image.BOX)) for c in range(4)], -1)
    n = max(1, int(round(STREAK * letters / SHRINK)))
    c = np.cumsum(np.pad(small, ((0, 0), (n // 2 + 1, n - n // 2), (0, 0))), axis=1)
    out = (c[:, n:n + small.shape[1]] - c[:, :small.shape[1]]) / n
    a = out[..., 3:4]
    rgb = np.where(a > 1e-6, out[..., :3] / np.maximum(a, 1e-6), 0.0)
    return Image.fromarray((np.concatenate([rgb, a], -1).clip(0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=os.path.join(ROOT, 'resources/options'))
    a = ap.parse_args()
    for name in NAMES:
        src = os.path.join(a.dir, 'title_%s.png' % name)
        if os.path.exists(src):
            blur(src).save(os.path.join(a.dir, 'title_%s_blur.png' % name), optimize=True)
            print(name)


if __name__ == '__main__':
    main()
