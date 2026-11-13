"""The OPTIONS pages' category titles (splashes) in the look of K's painted OPTIONS title.

K's PSD (title_headers.psd) holds each title's lettering as a group (the text layer + a second bevel layer, styled in
Photoshop) and, in OPTIONS only, the "Pattern" layer: glossy specks and tendrils inside the letters, blended Lighten.
This fills the other titles' letters with a pattern grown from OPTIONS' own (image quilting: overlapping blocks cut
along the seam where they differ least), letter by letter, each letter's speck density kept near OPTIONS' average, then
lays it over K's Photoshop export of the styled lettering with the same Lighten blend.

    python tools/options/make_splash.py title_headers.psd EXPORTS_DIR OUT_DIR [--seed N] [--only NAME] [--preview]

EXPORTS_DIR holds K's exports of the groups (Layers panel > Export As, PNG, trimmed is fine), named after the groups
(GAMEPLAY.png ...), OPTIONS without its pattern (its own Pattern layer goes back on: the result matches K's
options_title.png to 0.4 / 255). Writes OUT_DIR/title_<name>.png, every title on the PSD's canvas at half size, its
letters centred across it and at the PSD's height, so the titles swap in place (options_overlay.cpp title()).
--preview (no exports needed): an approximate base (dark fill, cream stroke, orange glow) to judge the pattern alone.
"""
import argparse, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage as nd

B, O = 384, 96           # quilting block and overlap (PSD pixels)
SCALE = 8                # candidates are compared at an eighth of the size
CANDIDATES = 400
BLOCK_BRIGHT = (0.08, 0.45)  # source blocks with this much speck in them (all-black ones would win every seam)
DENSITY = (0.15, 0.22)   # bright fraction of a letter (OPTIONS' letters: 0.10..0.24, mean 0.17)
BRIGHT = 60              # a pattern pixel counts as a speck above this (max channel)
SOURCE_INSET = 30        # source blocks stay this far inside OPTIONS' letters (clear of the bevel's red rim)
TARGET_INSET = 5         # the pattern stops this far inside the target letters
SCALE_OUT = 0.5          # written at half the PSD's size (8.72 px per layout unit: still 1.5x a 4K screen's)


def load(psd_path):
    from psd_tools import PSDImage
    psd = PSDImage.open(psd_path)
    W, H = psd.size
    words, pattern = {}, None
    for group in psd:
        if not group.is_group():
            continue
        for layer in group:
            im = layer.topil()
            if im is None:
                continue
            canvas = Image.new('RGBA', (W, H))
            canvas.paste(im.convert('RGBA'), (layer.left, layer.top))
            a = np.asarray(canvas)
            if layer.name == 'Pattern':
                pattern = a
            elif 'Bevel' not in layer.name and group.name not in words:
                words[group.name] = a[..., 3] > 127
    return words, pattern


def dihedral(block, k):
    if k & 4:
        block = block[:, ::-1]
    return np.rot90(block, k & 3)


def min_cut(err, axis):
    """A path of least error across an overlap (rows when axis 0, columns when axis 1): True = keep the old pixels."""
    e = err if axis == 0 else err.T
    h, w = e.shape
    cost = e.copy()
    for y in range(1, h):
        left = np.r_[np.inf, cost[y - 1, :-1]]
        right = np.r_[cost[y - 1, 1:], np.inf]
        cost[y] += np.minimum(np.minimum(left, cost[y - 1]), right)
    keep = np.zeros_like(e, bool)
    x = int(np.argmin(cost[-1]))
    for y in range(h - 1, -1, -1):
        keep[y, :x] = True
        if y:
            lo, hi = max(x - 1, 0), min(x + 2, w)
            x = lo + int(np.argmin(cost[y - 1, lo:hi]))
    return keep if axis == 0 else keep.T


class Source:
    def __init__(self, pattern, letters):
        # Premultiplied (no pattern = black = no effect under Lighten), faded out near OPTIONS' letter edges, so a block
        # may lie anywhere: what falls outside the letters is black.
        inner = nd.gaussian_filter(nd.binary_erosion(letters, iterations=SOURCE_INSET).astype(np.float32), 6.0)
        a = pattern[..., 3].astype(np.float32) / 255.0 * inner
        self.rgb = pattern[..., :3].astype(np.float32) * a[..., None]
        bright = (self.rgb.max(-1) > BRIGHT).astype(np.int32)
        integral = np.pad(bright.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
        ys, xs = np.mgrid[0:bright.shape[0] - B:16, 0:bright.shape[1] - B:16]
        frac = (integral[ys + B, xs + B] - integral[ys, xs + B] - integral[ys + B, xs] + integral[ys, xs]) / float(B * B)
        keep = (frac >= BLOCK_BRIGHT[0]) & (frac <= BLOCK_BRIGHT[1])
        self.positions = np.stack([ys[keep], xs[keep]], 1)
        small = Image.fromarray(np.clip(self.rgb, 0, 255).astype(np.uint8))
        self.small = np.asarray(small.resize((small.width // SCALE, small.height // SCALE), Image.BOX)).astype(np.float32)
        if not len(self.positions):
            sys.exit('no source blocks inside OPTIONS')

    def block(self, y, x, k, small=False):
        if small:
            s, b = SCALE, B // SCALE
            return dihedral(self.small[y // s:y // s + b, x // s:x // s + b], k)
        return dihedral(self.rgb[y:y + B, x:x + B], k)


def quilt(source, mask, rng):
    """Fills mask's bounding box (one letter) block by block; returns RGB (float) over the box and the box."""
    ys, xs = np.nonzero(mask)
    y0, x0 = max(ys.min() - O, 0), max(xs.min() - O, 0)
    y1, x1 = ys.max() + O, xs.max() + O
    step = B - O
    rows = int(np.ceil((y1 - y0 - O) / step)) or 1
    cols = int(np.ceil((x1 - x0 - O) / step)) or 1
    h, w = rows * step + O, cols * step + O
    out = np.zeros((h, w, 3), np.float32)
    small = np.zeros((h // SCALE + 1, w // SCALE + 1, 3), np.float32)
    region = np.zeros((h, w), bool)
    region[:min(h, mask.shape[0] - y0), :min(w, mask.shape[1] - x0)] = mask[y0:y0 + h, x0:x0 + w]
    sb, so = B // SCALE, O // SCALE
    used = []  # source positions already in this letter: never again (a block beside its own mirror image is a seam
               # with no error, and shows as a symmetric pattern)
    for r in range(rows):
        for c in range(cols):
            by, bx = r * step, c * step
            if not region[by:by + B, bx:bx + B].any():
                continue
            pick = rng.integers(0, len(source.positions), CANDIDATES)
            if used:
                near = np.zeros(CANDIDATES, bool)
                for uy, ux in used:
                    near |= (np.abs(source.positions[pick, 0] - uy) < B) & (np.abs(source.positions[pick, 1] - ux) < B)
                if (~near).any():
                    pick = pick[~near]
            orient = rng.integers(0, 8, len(pick))
            sy, sx = by // SCALE, bx // SCALE
            errors = np.zeros(len(pick))
            for i in range(len(pick)):
                y, x = source.positions[pick[i]]
                cand = source.block(y, x, orient[i], small=True)
                e = 0.0
                if c:
                    e += ((cand[:, :so] - small[sy:sy + sb, sx:sx + so]) ** 2).sum()
                if r:
                    e += ((cand[:so, :] - small[sy:sy + so, sx:sx + sb]) ** 2).sum()
                errors[i] = e
            good = np.nonzero(errors <= errors.min() * 1.15 + 1e-6)[0]
            i = rng.choice(good)
            y, x = source.positions[pick[i]]
            used.append((y, x))
            new = source.block(y, x, orient[i])
            keep = np.zeros((B, B), bool)
            old = out[by:by + B, bx:bx + B]
            if c:
                keep[:, :O] |= min_cut(((new[:, :O] - old[:, :O]) ** 2).sum(-1), 0)
            if r:
                keep[:O, :] |= min_cut(((new[:O, :] - old[:O, :]) ** 2).sum(-1), 1)
            out[by:by + B, bx:bx + B] = np.where(keep[..., None], old, new)
            small[sy:sy + sb, sx:sx + sb] = source.block(y, x, orient[i], small=True)
    return out, (y0, x0)


def pattern_for(word_mask, source, seed):
    """Letter by letter, each re-grown (new seeds) until its speck density sits in DENSITY."""
    H, W = word_mask.shape
    layer = np.zeros((H, W, 3), np.float32)
    labels, n = nd.label(word_mask)
    inner = nd.binary_erosion(word_mask, iterations=TARGET_INSET)
    for i in range(1, n + 1):
        letter = labels == i
        best = None
        for attempt in range(12):
            rng = np.random.default_rng(seed * 1000 + i * 37 + attempt)
            fill, (y0, x0) = quilt(source, letter, rng)
            h, w = min(fill.shape[0], H - y0), min(fill.shape[1], W - x0)
            fill = fill[:h, :w]
            m = letter[y0:y0 + h, x0:x0 + w]
            density = ((fill.max(-1) > BRIGHT) & m).sum() / max(m.sum(), 1)
            miss = 0.0 if DENSITY[0] <= density <= DENSITY[1] else min(abs(density - DENSITY[0]), abs(density - DENSITY[1]))
            if best is None or miss < best[0]:
                best = (miss, fill, y0, x0, density)
            if miss == 0.0:
                break
        _, fill, y0, x0, density = best
        h, w = fill.shape[:2]
        sel = letter[y0:y0 + h, x0:x0 + w]
        layer[y0:y0 + h, x0:x0 + w][sel] = fill[sel]
        print('  letter %d: density %.3f' % (i, density))
    alpha = nd.gaussian_filter(inner.astype(np.float32), 1.5)
    return layer, alpha


def approximate_base(mask):
    """Stand-in for K's export: near-black fill, a 14 px cream stroke, an orange glow."""
    d_out = nd.distance_transform_edt(~mask)
    glow = np.clip(1.0 - d_out / 140.0, 0, 1) ** 1.6
    rgb = np.zeros(mask.shape + (3,), np.float32)
    a = np.zeros(mask.shape, np.float32)
    rgb[:] = np.array([171, 57, 17], np.float32)
    a = glow
    stroke = (d_out > 0) & (d_out <= 14)
    rgb[stroke] = (252, 190, 101)
    a[stroke] = 1
    rgb[mask] = (10, 4, 2)
    a[mask] = 1
    return np.dstack([rgb, a * 255]).astype(np.float32)


def register(export, mask):
    """Where a trimmed export sits on the PSD canvas: the offset that best lines its dark letter fill up with mask."""
    dark = (export[..., :3].max(-1) < 40) & (export[..., 3] > 200)
    ys, xs = np.nonzero(mask)
    dy, dx = np.nonzero(dark)
    guess = (ys.min() - dy.min(), xs.min() - dx.min())
    best = None
    for oy in range(guess[0] - 40, guess[0] + 41, 2):
        for ox in range(guess[1] - 40, guess[1] + 41, 2):
            shifted = np.zeros_like(mask)
            h, w = dark.shape
            ty0, tx0 = max(oy, 0), max(ox, 0)
            ty1, tx1 = min(oy + h, mask.shape[0]), min(ox + w, mask.shape[1])
            if ty1 <= ty0 or tx1 <= tx0:
                continue
            shifted[ty0:ty1, tx0:tx1] = dark[ty0 - oy:ty1 - oy, tx0 - ox:tx1 - ox]
            score = (shifted & mask).sum() - (shifted & ~mask).sum()
            if best is None or score > best[0]:
                best = (score, oy, ox)
    return best[1], best[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('psd')
    ap.add_argument('exports')
    ap.add_argument('out')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--only')
    ap.add_argument('--preview', action='store_true')
    a = ap.parse_args()
    words, pattern = load(a.psd)
    if pattern is None or 'OPTIONS' not in words:
        sys.exit('the PSD needs the OPTIONS group with its Pattern layer')
    source = Source(pattern, words['OPTIONS'])
    print('source blocks:', len(source.positions))
    H, W = words['OPTIONS'].shape
    os.makedirs(a.out, exist_ok=True)
    for name, mask in words.items():
        if a.only and name != a.only:
            continue
        print(name)
        if a.preview:
            base, oy, ox = approximate_base(mask), 0, 0
        else:
            path = os.path.join(a.exports, name + '.png')
            if not os.path.exists(path):
                print('  no export', path)
                continue
            base = np.asarray(Image.open(path).convert('RGBA')).astype(np.float32)
            oy, ox = register(base, mask)
            print('  export at', oy, ox)
        # The letters centred across the canvas (OPTIONS sits off-centre in the PSD).
        xs = np.nonzero(mask.any(0))[0]
        shift = W // 2 - (xs.min() + xs.max() + 1) // 2
        full = np.zeros((H, W, 4), np.float32)
        h, w = min(base.shape[0], H - oy), min(base.shape[1], W - ox)
        full[oy:oy + h, ox:ox + w] = base[:h, :w]
        if name == 'OPTIONS':
            pat = pattern[..., :3].astype(np.float32)
            alpha = pattern[..., 3].astype(np.float32) / 255.0
        else:
            pat, alpha = pattern_for(mask, source, a.seed)
        lit = np.maximum(full[..., :3], pat)  # Lighten
        k = alpha[..., None] * (full[..., 3:4] > 0)
        full[..., :3] = full[..., :3] * (1 - k) + lit * k
        full = np.roll(full, shift, axis=1)
        im = Image.fromarray(np.clip(full + 0.5, 0, 255).astype(np.uint8), 'RGBA')
        im = im.resize((round(W * SCALE_OUT), round(H * SCALE_OUT)), Image.LANCZOS)
        im.save(os.path.join(a.out, 'title_%s.png' % name.lower()), optimize=True)
        print('  shift %d, letters %d..%d' % (shift, xs.min() + shift, xs.max() + shift))


if __name__ == '__main__':
    main()
