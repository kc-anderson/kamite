"""The OPTIONS pages' category titles (splashes) in the look of K's painted OPTIONS title.

K's PSD (title_headers.psd) holds each title's lettering as a group (the text layer + a second bevel layer, styled in
Photoshop) and, in OPTIONS only, the "Pattern" layer: glossy specks and tendrils inside the letters, blended Lighten.
This fills the other titles' letters with pieces of that pattern (image quilting: overlapping blocks cut along the seam
where they differ least) and lays them over K's Photoshop export of the styled lettering with the same Lighten blend.

- No piece of the pattern is used twice in one title (K, Nov 13: repeats in nearby letters read as a pattern).
- CONTROLS follows the game's own CONTROLS title (the Options screen's controller page, decoded from your disc's
  idsubscroption_us.idd): its specks, letter by letter, guide where the pieces go (K, Nov 13).
- The others: each letter's speck density near OPTIONS' (as much as the pattern has without repeats).

    python tools/options/make_splash.py title_headers.psd EXPORTS_DIR DISC_DIR OUT_DIR [--seed N] [--only NAME]

EXPORTS_DIR holds K's exports of the groups (Layers panel > Export As, PNG, trimmed is fine), named after the groups
(GAMEPLAY.png ...), OPTIONS without its pattern (its own Pattern layer goes back on: the result matches K's
options_title.png to 0.4 / 255). DISC_DIR: the extracted disc (God Hand Extract). Writes OUT_DIR/title_<name>.png,
every title on the PSD's canvas at half size, its letters centred across it and at the PSD's height, so the titles
swap in place (options_overlay.cpp title()).
"""
import argparse, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage as nd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps2tex

SCALE = 4                 # candidates are compared at a quarter of the size
CANDIDATES = 500
BRIGHT = 60               # a pattern pixel counts as a speck above this (max channel)
DENSITY = (0.10, 0.15)    # free titles: a letter's speck fraction (OPTIONS' letters 0.10..0.24; the pattern has too
                          # little to fill the long titles at OPTIONS' mean without repeats)
SOURCE_INSET = 30         # OPTIONS' pattern fades out this far inside its letters (clear of the bevel's red rim)
TARGET_INSET = 5          # the pattern stops this far inside the target letters
REUSE_OVERLAP = 0.05      # a block may overlap a used one by at most this much of its area (when it has specks)
GUIDE_WEIGHT = 3.0        # guided titles: how much the guide outweighs the seams
CLUSTER = 16              # the game's specks this close (PSD pixels) count as one speck
PIECE_SPLIT = 2500        # OPTIONS' speck networks bigger than this (px) are cut into pieces about this size
PIECE_SCALE = (0.7, 1.4)  # how far a piece may be scaled to fit a speck
PIECE_GROW = 2.2          # a piece covers this many times its speck's area (K's specks are fuller than the texels)
SCALE_OUT = 0.5           # written at half the PSD's size (8.72 px per layout unit: still 1.5x a 4K screen's)
STROKE = 14               # K's outside stroke (PSD pixels): the game's ring maps onto it
# The game's own CONTROLS: the controller page (the layout's second T32), its painted title.
GAME_CONTROLS = ('id/local/us/idsubscroption_us.idd', 1, (8, 80, 10, 254))


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


def shrink(a):
    h, w = a.shape[:2]
    a = a[:h // SCALE * SCALE, :w // SCALE * SCALE]
    return a.reshape(h // SCALE, SCALE, w // SCALE, SCALE, *a.shape[2:]).mean((1, 3))


class Source:
    """OPTIONS' pattern, premultiplied (no pattern = black = no effect under Lighten) and faded out near OPTIONS' letter
    edges, so a block may lie anywhere: what falls outside the letters is black."""

    def __init__(self, pattern, letters):
        inner = nd.gaussian_filter(nd.binary_erosion(letters, iterations=SOURCE_INSET).astype(np.float32), 6.0)
        a = pattern[..., 3].astype(np.float32) / 255.0 * inner
        self.rgb = pattern[..., :3].astype(np.float32) * a[..., None]
        self.bright = (self.rgb.max(-1) > BRIGHT).astype(np.float32)
        self.guide = self.bright
        self.small = shrink(self.rgb)
        self.small_guide = shrink(self.guide)
        self.integral = np.pad(self.bright.cumsum(0).cumsum(1), ((1, 0), (1, 0)))

    def positions(self, B, lo=0.0, hi=1.0):
        ys, xs = np.mgrid[0:self.rgb.shape[0] - B:16, 0:self.rgb.shape[1] - B:16]
        I = self.integral
        frac = (I[ys + B, xs + B] - I[ys, xs + B] - I[ys + B, xs] + I[ys, xs]) / float(B * B)
        keep = (frac >= lo) & (frac <= hi)
        return np.stack([ys[keep], xs[keep]], 1), frac[keep]

    def block(self, y, x, k, B, small=False, guide=False):
        if small:
            s, b = SCALE, B // SCALE
            src = self.small_guide if guide else self.small
            return dihedral(src[y // s:y // s + b, x // s:x // s + b], k)
        return dihedral(self.rgb[y:y + B, x:x + B], k)


def quilt(source, mask, rng, B, O, positions, specky, used, guide=None):
    """Fills mask's bounding box (one letter) block by block; returns RGB over the box, the box's corner and the source
    blocks taken. used: blocks already in the title (y, x): none is overlapped by more than REUSE_OVERLAP."""
    ys, xs = np.nonzero(mask)
    y0, x0 = max(ys.min() - O, 0), max(xs.min() - O, 0)
    y1, x1 = ys.max() + O, xs.max() + O
    step = B - O
    rows = max(int(np.ceil((y1 - y0 - O) / step)), 1)
    cols = max(int(np.ceil((x1 - x0 - O) / step)), 1)
    h, w = rows * step + O, cols * step + O
    out = np.zeros((h, w, 3), np.float32)
    small = np.zeros((h // SCALE + 1, w // SCALE + 1, 3), np.float32)
    region = np.zeros((h, w), bool)
    hh, ww = min(h, mask.shape[0] - y0), min(w, mask.shape[1] - x0)
    region[:hh, :ww] = mask[y0:y0 + hh, x0:x0 + ww]
    gsmall = None
    if guide is not None:
        g = np.zeros((h, w), np.float32)
        g[:hh, :ww] = guide[y0:y0 + hh, x0:x0 + ww]
        gsmall = shrink(g)
    sb, so = B // SCALE, O // SCALE
    taken = []
    limit = REUSE_OVERLAP * B * B
    for r in range(rows):
        for c in range(cols):
            by, bx = r * step, c * step
            if not region[by:by + B, bx:bx + B].any():
                continue
            pick = rng.integers(0, len(positions), CANDIDATES)
            if used:
                u = np.array(used)
                py, px = positions[pick, 0][:, None], positions[pick, 1][:, None]
                over = np.clip(B - np.abs(py - u[None, :, 0]), 0, None) * np.clip(B - np.abs(px - u[None, :, 1]), 0, None)
                ok = ~((over > limit).any(1) & specky[pick])
                if ok.any():
                    pick = pick[ok]
            orient = rng.integers(0, 8, len(pick))
            sy, sx = by // SCALE, bx // SCALE
            errors = np.zeros(len(pick))
            for i in range(len(pick)):
                y, x = positions[pick[i]]
                cand = source.block(y, x, orient[i], B, small=True)
                e = 0.0
                if c:
                    e += ((cand[:, :so] - small[sy:sy + sb, sx:sx + so]) ** 2).sum()
                if r:
                    e += ((cand[:so, :] - small[sy:sy + so, sx:sx + sb]) ** 2).sum()
                if gsmall is not None:
                    gc = source.block(y, x, orient[i], B, small=True, guide=True)
                    t = gsmall[sy:sy + sb, sx:sx + sb]
                    # Specks where the game has none cost most; the game's specks left bare cost less. In the seam
                    # error's units (a fully wrong overlap pixel = 3 x 255^2).
                    e += GUIDE_WEIGHT * 3 * 255.0 ** 2 * ((gc * (1 - t)).sum() * 1.0 + (t * (1 - gc)).sum() * 0.5)
                errors[i] = e
            good = np.nonzero(errors <= errors.min() * 1.15 + 1e-6)[0]
            i = rng.choice(good)
            y, x = positions[pick[i]]
            if specky[pick[i]]:
                taken.append((y, x))
                used.append((y, x))
            new = source.block(y, x, orient[i], B)
            keep = np.zeros((B, B), bool)
            old = out[by:by + B, bx:bx + B]
            if c:
                keep[:, :O] |= min_cut(((new[:, :O] - old[:, :O]) ** 2).sum(-1), 0)
            if r:
                keep[:O, :] |= min_cut(((new[:O, :] - old[:O, :]) ** 2).sum(-1), 1)
            out[by:by + B, bx:bx + B] = np.where(keep[..., None], old, new)
            small[sy:sy + sb, sx:sx + sb] = source.block(y, x, orient[i], B, small=True)
    return out, (y0, x0), taken


def pattern_for(word_mask, source, seed, guide=None):
    """Letter by letter, left to right, no source block twice in the word. Free titles: each letter re-grown (new seeds)
    until its speck density sits in DENSITY; guided: the guide decides."""
    H, W = word_mask.shape
    layer = np.zeros((H, W, 3), np.float32)
    labels, n = nd.label(word_mask)
    order = sorted(range(1, n + 1), key=lambda i: np.nonzero((labels == i).any(0))[0].min())
    inner = nd.binary_erosion(word_mask, iterations=TARGET_INSET)
    if guide is None:
        B, O = 256, 64
        positions, frac = source.positions(B, 0.03, 0.45)
    else:
        B, O = 128, 32
        positions, frac = source.positions(B)
    specky = frac > 0.01
    used = []
    for i in order:
        letter = labels == i
        best = None
        for attempt in range(1 if guide is not None else 10):
            rng = np.random.default_rng(seed * 1000 + i * 37 + attempt)
            trial = list(used)
            fill, (y0, x0), taken = quilt(source, letter, rng, B, O, positions, specky, trial, guide)
            h, w = min(fill.shape[0], H - y0), min(fill.shape[1], W - x0)
            fill = fill[:h, :w]
            m = letter[y0:y0 + h, x0:x0 + w]
            density = ((fill.max(-1) > BRIGHT) & m).sum() / max(m.sum(), 1)
            miss = 0.0 if guide is not None or DENSITY[0] <= density <= DENSITY[1] else \
                min(abs(density - DENSITY[0]), abs(density - DENSITY[1]))
            if best is None or miss < best[0]:
                best = (miss, fill, y0, x0, density, taken)
            if miss == 0.0:
                break
        _, fill, y0, x0, density, taken = best
        used.extend(taken)
        h, w = fill.shape[:2]
        sel = letter[y0:y0 + h, x0:x0 + w]
        layer[y0:y0 + h, x0:x0 + w][sel] = fill[sel]
        print('  letter %d: density %.3f' % (i, density))
    alpha = nd.gaussian_filter(inner.astype(np.float32), 1.5)
    return layer, alpha


def game_boxes(stroke):
    """The game title's letters: stroke columns, split where the stroke stops for two columns or more."""
    on = stroke.any(0)
    segs = []
    for x in range(len(on)):
        if on[x] and (not segs or x - segs[-1][1] > 1):
            segs.append([x, x + 1])
        elif on[x]:
            segs[-1][1] = x + 1
    boxes = []
    for a, b in segs:
        ys = np.nonzero(stroke[:, a:b].any(1))[0]
        boxes.append((a, b, ys.min(), ys.max() + 1))
    return boxes


def map_letters(word_mask, img, boxes):
    """A texel image of the game title onto the word's letters, box to box (its stroke ring onto K's), nearest."""
    labels, n = nd.label(word_mask)
    objs = sorted(nd.find_objects(labels), key=lambda o: o[1].start)
    if len(objs) != len(boxes):
        sys.exit('the game title has %d letters, the PSD %d' % (len(boxes), len(objs)))
    out = np.zeros(word_mask.shape, img.dtype)
    for o, (gx0, gx1, gy0, gy1) in zip(objs, boxes):
        oy0, oy1 = o[0].start - STROKE, o[0].stop + STROKE
        ox0, ox1 = o[1].start - STROKE, o[1].stop + STROKE
        ys, xs = np.mgrid[oy0:oy1, ox0:ox1]
        u = np.clip((gx0 + (xs + 0.5 - ox0) / (ox1 - ox0) * (gx1 - gx0)).astype(int), 0, img.shape[1] - 1)
        v = np.clip((gy0 + (ys + 0.5 - oy0) / (oy1 - oy0) * (gy1 - gy0)).astype(int), 0, img.shape[0] - 1)
        out[oy0:oy1, ox0:ox1] = np.maximum(out[oy0:oy1, ox0:ox1], img[v, u])
    return out


def game_guide(disc_dir, word_mask):
    """Where the game's own CONTROLS has specks, on the word's letters: its bright speck colours (not the stroke ring,
    not the red bevel, not the thin lit lines along its edges) mapped letter by letter, clear of K's edges, 0..1."""
    path, index, (r0, r1, c0, c1) = GAME_CONTROLS
    g = ps2tex.t32_textures(os.path.join(disc_dir, path))[index][r0:r1, c0:c1].astype(np.float32)
    R, G = g[..., 0], g[..., 1]
    stroke = (R > 190) & (G > 120)
    colour = (g.max(-1) > 90) & (G > 0.35 * R) & ~stroke
    boxes = game_boxes(stroke)
    ring = map_letters(word_mask, stroke, boxes)
    speck = map_letters(word_mask, colour, boxes)
    body = nd.binary_erosion(word_mask, iterations=22) & ~nd.binary_dilation(ring, iterations=12)
    speck &= body
    labels, n = nd.label(speck)
    for i, o in enumerate(nd.find_objects(labels), 1):
        h, w = o[0].stop - o[0].start, o[1].stop - o[1].start
        if min(h, w) <= 20 and max(h, w) >= 70:  # a lit line along an edge, not a speck
            speck[o][labels[o] == i] = False
    return speck


def pieces_of(source, rng):
    """OPTIONS' pattern cut into separate specks: each connected bright shape (big networks split by k-means) with a
    soft margin of its own shading. Returns [(rgb, alpha, area, angle)]."""
    labels, n = nd.label(source.bright > 0, structure=np.ones((3, 3)))
    out = []
    for i, o in enumerate(nd.find_objects(labels), 1):
        comp = labels[o] == i
        area = int(comp.sum())
        if area < 40:
            continue
        ys, xs = np.nonzero(comp)
        k = max(1, int(round(area / PIECE_SPLIT)))
        if k > 1:
            pts = np.stack([ys, xs], 1).astype(np.float32)
            centres = pts[rng.choice(len(pts), k, replace=False)]
            for _ in range(12):
                assign = np.argmin(((pts[:, None] - centres[None]) ** 2).sum(-1), 1)
                centres = np.array([pts[assign == j].mean(0) if (assign == j).any() else centres[j] for j in range(k)])
        else:
            assign = np.zeros(len(ys), int)
        for j in range(k):
            sel = assign == j
            if sel.sum() < 40:
                continue
            py, px = ys[sel] + o[0].start, xs[sel] + o[1].start
            pad = 12
            y0, y1 = max(py.min() - pad, 0), min(py.max() + pad + 1, source.rgb.shape[0])
            x0, x1 = max(px.min() - pad, 0), min(px.max() + pad + 1, source.rgb.shape[1])
            m = np.zeros((y1 - y0, x1 - x0), np.float32)
            m[py - y0, px - x0] = 1
            m = nd.gaussian_filter(nd.binary_dilation(m > 0, iterations=6).astype(np.float32), 2.5)
            cy, cx = py.mean() - y0, px.mean() - x0
            cov = np.cov(np.stack([py - py.mean(), px - px.mean()]))
            angle = 0.5 * np.degrees(np.arctan2(2 * cov[0, 1], cov[1, 1] - cov[0, 0])) if sel.sum() > 2 else 0.0
            out.append((source.rgb[y0:y1, x0:x1].copy(), m, int(sel.sum()), angle, (cy, cx)))
    return out


def sprite_pattern(word_mask, source, specks, seed):
    """One piece of OPTIONS' pattern on each of the game's specks (clustered), sized and turned to fit it, no piece
    twice in the word (when they run out, a small one may come back three or more letters away)."""
    rng = np.random.default_rng(seed)
    pieces = pieces_of(source, rng)
    H, W = word_mask.shape
    layer = np.zeros((H, W, 3), np.float32)
    letters, _ = nd.label(word_mask)
    order = {lab: k for k, lab in enumerate(sorted(range(1, letters.max() + 1),
                                                   key=lambda i: np.nonzero((letters == i).any(0))[0].min()))}
    clusters, n = nd.label(nd.binary_dilation(specks, iterations=CLUSTER // 2))
    targets = []
    for i, o in enumerate(nd.find_objects(clusters), 1):
        sel = (clusters[o] == i) & specks[o]
        ys, xs = np.nonzero(sel)
        if len(ys) < 20:
            continue
        ys, xs = ys + o[0].start, xs + o[1].start
        cov = np.cov(np.stack([ys - ys.mean(), xs - xs.mean()])) if len(ys) > 2 else np.eye(2)
        angle = 0.5 * np.degrees(np.arctan2(2 * cov[0, 1], cov[1, 1] - cov[0, 0]))
        letter = letters[int(ys.mean()), int(xs.mean())] or letters[ys[0], xs[0]]
        targets.append((len(ys), ys.mean(), xs.mean(), angle, order.get(letter, 0)))
    targets.sort(key=lambda t: -t[0])
    used = {}  # piece -> letters it went to
    areas = np.array([p[2] for p in pieces], np.float32)
    for area, ty, tx, tangle, li in targets:
        want = area * PIECE_GROW
        scale = np.sqrt(want / areas)
        fit = np.abs(np.log(np.clip(scale, *PIECE_SCALE) / scale))
        free = np.array([k not in used for k in range(len(pieces))])
        far = np.array([all(abs(li - l) >= 3 for l in used.get(k, [])) and areas[k] < 600 for k in range(len(pieces))])
        cand = np.nonzero(free)[0] if free.any() else np.nonzero(far)[0]
        if not len(cand):
            continue
        best = cand[np.argsort(fit[cand])[:4]]
        k = int(rng.choice(best))
        used.setdefault(k, []).append(li)
        rgb, m, parea, pangle, (cy, cx) = pieces[k]
        s = float(np.clip(scale[k], *PIECE_SCALE))
        turn = tangle - pangle + (180.0 if rng.random() < 0.5 else 0.0)
        flip = rng.random() < 0.5
        if flip:
            rgb, m, cx, turn = rgb[:, ::-1], m[:, ::-1], rgb.shape[1] - 1 - cx, -turn + 2 * tangle
        rgba = np.dstack([rgb, m])
        rgba = nd.zoom(rgba, (s, s, 1), order=1)
        # turn about the piece's centre of mass
        big = max(rgba.shape[:2]) * 2
        canvas = np.zeros((big, big, 4), np.float32)
        oy, ox = big // 2 - int(cy * s), big // 2 - int(cx * s)
        canvas[oy:oy + rgba.shape[0], ox:ox + rgba.shape[1]] = rgba
        canvas = nd.rotate(canvas, turn, reshape=False, order=1)
        y0, x0 = int(round(ty)) - big // 2, int(round(tx)) - big // 2
        ya, xa = max(y0, 0), max(x0, 0)
        yb, xb = min(y0 + big, H), min(x0 + big, W)
        if yb <= ya or xb <= xa:
            continue
        piece = canvas[ya - y0:yb - y0, xa - x0:xb - x0]
        a = np.clip(piece[..., 3:4], 0, 1)
        layer[ya:yb, xa:xb] = np.maximum(layer[ya:yb, xa:xb], piece[..., :3] * a)
    inner = nd.binary_erosion(word_mask, iterations=TARGET_INSET)
    print('  %d specks, %d pieces (%d used)' % (len(targets), len(pieces), len(used)))
    return layer, nd.gaussian_filter(inner.astype(np.float32), 1.5)


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
    ap.add_argument('disc')
    ap.add_argument('out')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--only')
    a = ap.parse_args()
    words, pattern = load(a.psd)
    if pattern is None or 'OPTIONS' not in words:
        sys.exit('the PSD needs the OPTIONS group with its Pattern layer')
    source = Source(pattern, words['OPTIONS'])
    H, W = words['OPTIONS'].shape
    os.makedirs(a.out, exist_ok=True)
    for name, mask in words.items():
        if a.only and name != a.only:
            continue
        print(name)
        path = os.path.join(a.exports, name + '.png')
        if not os.path.exists(path):
            print('  no export', path)
            continue
        base = np.asarray(Image.open(path).convert('RGBA')).astype(np.float32)
        oy, ox = register(base, mask)
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
            if name == 'CONTROLS':
                pat, alpha = sprite_pattern(mask, source, game_guide(a.disc, mask), a.seed)
            else:
                pat, alpha = pattern_for(mask, source, a.seed)
        lit = np.maximum(full[..., :3], pat)  # Lighten
        k = alpha[..., None] * (full[..., 3:4] > 0)
        full[..., :3] = full[..., :3] * (1 - k) + lit * k
        full = np.roll(full, shift, axis=1)
        im = Image.fromarray(np.clip(full + 0.5, 0, 255).astype(np.uint8), 'RGBA')
        im = im.resize((round(W * SCALE_OUT), round(H * SCALE_OUT)), Image.LANCZOS)
        im.save(os.path.join(a.out, 'title_%s.png' % name.lower()), optimize=True)


if __name__ == '__main__':
    main()
