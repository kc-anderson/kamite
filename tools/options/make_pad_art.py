"""The Button Mapping page's art (CONTROLS > BUTTON MAPPING), in the look of the game's own controller page
(claude/button-mapping.md "The original controller page"). Our own drawings from measured colours and K's pad renders:
nothing of the game's art ships.

    python tools/options/make_pad_art.py RENDERS_DIR [--seed N] [--out DIR]

RENDERS_DIR holds K's renders (pad_xbox.png, pad_playstation.png, pad_nintendo.png: front views on transparent
backgrounds). Writes into resources/options/mapping (or --out):
- pad_<name>.png: the render (the Xbox one lightly blurred: K, it read over-sharpened); flames_<name>.png: the flame
  outline round its silhouette (half the render's resolution, MARGIN of its height round it);
- pad_<name>.txt: the page's data for it (mapping_page.cpp loadArt): the images, each button's point, the columns'
  rows top to bottom and the silhouette's ends per band (the label panels stop just short of it, as the original's do);
- leader.png, underscore.png (+ _lit): the lines' grungy brush strokes (laid along each line), normal and selected;
  a leader tapers to a point at the button;
- panel.png: the label panels' wash (opaque at the outer end, nothing at the inner);
- stick_<set>_<left|right>.png: the pack's stick-press icons without the click arrow (K, Nov 3: the stick icons are the
  click images' side view, same frame), as prompt_hd.cpp makes them.
"""
import argparse, os, shutil
import numpy as np
from PIL import Image
from scipy import ndimage as nd

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MARGIN = 0.16   # the flames' room round the render (of its height)
BANDS = 64      # silhouette bands

# Each pad: its buttons (render pixels: x, y, how far short of the point its line stops) and the columns' rows. A row
# shows only when it has something to say (mapping_page.cpp); the order keeps the lines from crossing. A stick's click
# line ends on its cap, its own line on the ring below, so the two never run together. via: a bend the line takes on its
# way (the left face button's line passes under the bottom one, as the original's square line does).
PADS = {
    'xbox': dict(
        points=dict(south=(1128, 535, 42), east=(1238, 432, 42), west=(1018, 432, 42), north=(1130, 330, 42),
                    l1=(255, 205, 0), r1=(1200, 205, 0), l2=(300, 112, 0), r2=(1165, 112, 0),
                    l3=(310, 412, 18), r3=(918, 637, 18), start=(880, 428, 26), select=(565, 430, 26),
                    dpad=(500, 665, 70), lstick=(310, 478, 62), rstick=(918, 703, 62)),
        via=dict(west=(1095, 612)),
        left=['l2', 'l1', 'l3', 'lstick', 'dpad'],
        right=['start', 'r2', 'r1', 'north', 'east', 'south', 'west', 'r3', 'rstick']),
    'playstation': dict(
        points=dict(south=(1265, 563, 48), east=(1390, 440, 48), west=(1140, 440, 48), north=(1268, 318, 48),
                    l1=(250, 140, 0), r1=(1335, 140, 0), l2=(255, 45, 0), r2=(1330, 45, 0),
                    l3=(552, 640, 18), r3=(1027, 640, 18), start=(935, 432, 30), select=(648, 432, 30),
                    dpad=(322, 438, 100), lstick=(552, 706, 62), rstick=(1027, 706, 62)),
        via=dict(west=(1212, 645)),
        left=['l2', 'l1', 'dpad', 'l3', 'lstick'],
        right=['start', 'r2', 'r1', 'north', 'east', 'south', 'west', 'r3', 'rstick']),
    'nintendo': dict(
        points=dict(south=(1193, 515, 42), east=(1313, 422, 42), west=(1067, 422, 42), north=(1190, 328, 42),
                    l1=(300, 110, 0), r1=(1240, 112, 0), l2=(318, 52, 0), r2=(1222, 52, 0),
                    l3=(530, 615, 16), r3=(995, 615, 16), start=(865, 415, 25), select=(670, 415, 25),
                    dpad=(338, 418, 85), lstick=(530, 672, 55), rstick=(995, 672, 55)),
        via=dict(west=(1150, 592)),
        left=['l2', 'l1', 'dpad', 'l3', 'lstick'],
        right=['start', 'r2', 'r1', 'north', 'east', 'south', 'west', 'r3', 'rstick']),
}

# The flames (K, Nov 14, after the original): a pale yellow rim (yellow, not white) on a hot orange band, flame butts
# clumped round the bottom and lower sides, ragged feathered tongues spread evenly round the outline (every SPACING
# lines), rising and curling up, longest where the outline faces up, tapering off a short way out; a broad orange glow.
FIRE = dict(spacing=6.5, L0=8.0, Lup=20.0, Lside=7.0, W=4.4, out=1.2, bend=0.3, fade=0.45, butt=5.0, ragged=1.0,
            band=1.6, band_heat=0.75, rim=1.6, rim_a=0.9, rim_rgb=(255, 214, 85), glow=12.0, glow_a=0.55, soft=0.35,
            alpha=0.88, warp=3.5)
PAD_LINES = 190.0     # the pad's height on the page (options_overlay.cpp kMapPadHeight)
XBOX_BLUR = 1.1       # K: the Xbox render reads over-sharpened next to the softened text (render pixels)

# The pack's sets (resources/prompts/xelu/<folder>) and their stick-press images.
STICK_SETS = {'xbox360': '360', 'ps3': 'PS3', 'switch': 'Switch', 'steamdeck': 'SteamDeck'}


def fbm(shape, rng, scales, weights, stretch=(1.0, 1.0)):
    """Smooth value noise: random grids at a few scales, bicubically enlarged and summed."""
    h, w = shape
    out = np.zeros(shape, np.float32)
    for s, wt in zip(scales, weights):
        gh, gw = max(2, int(h / (s * stretch[0])) + 2), max(2, int(w / (s * stretch[1])) + 2)
        z = nd.zoom(rng.random((gh, gw)).astype(np.float32), (h / (gh - 1), w / (gw - 1)), order=3)[:h, :w]
        out += wt * np.pad(z, ((0, max(0, h - z.shape[0])), (0, max(0, w - z.shape[1]))), mode='edge')
    return out / sum(weights)


def ramp(t, stops):
    t = np.clip(t, 0, 1)
    out = np.zeros(t.shape + (3,), np.float32)
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        m = (t >= t0) & (t <= t1)
        f = ((t - t0) / max(t1 - t0, 1e-6))[m][:, None]
        out[m] = np.array(c0) * (1 - f) + np.array(c1) * f
    return out


def fbm_n(shape, rng, scales, weights, stretch=(1.0, 1.0)):
    """fbm normalised to mean 0.5, about a sixth spread."""
    z = fbm(shape, rng, scales, weights, stretch)
    return (z - z.mean()) / (z.std() * 3.0) + 0.5


def flames(render, seed):
    """The flame outline (RGBA, half the render's resolution, MARGIN of its height round it): tongues and butts placed
    along the outline (FIRE), pushed about by noise and eaten at the tips, over a hot band, a rim and a glow."""
    p = FIRE
    W, H = render.size
    scale = 0.5
    w, h = int(W * scale), int(H * scale)
    a = np.asarray(render.resize((w, h), Image.LANCZOS))[..., 3].astype(np.float32) / 255
    M = int(round(MARGIN * H * scale))
    a = np.pad(a, M)
    mask = a > 0.5
    hh, ww = a.shape
    u = h / PAD_LINES  # pixels a line
    dist = nd.distance_transform_edt(~mask).astype(np.float32) / u
    inside = nd.distance_transform_edt(mask).astype(np.float32) / u
    gy, gx = np.gradient(nd.gaussian_filter(dist - inside, 3 * u))
    nrm = np.hypot(gx, gy) + 1e-6
    nx, ny = gx / nrm, gy / nrm  # the outward normal
    rng = np.random.default_rng(seed)
    rows = np.where(mask.any(axis=1))[0]
    top, bot = rows[0], rows[-1]
    # Outline points, evenly spread: in random order, each kept only clear of the kept ones by SPACING.
    pts = np.argwhere(mask & ~nd.binary_erosion(mask))
    rng.shuffle(pts)
    sp = p['spacing'] * u
    grid, keep = {}, []
    for y, x in pts:
        cy, cx = int(y // sp), int(x // sp)
        if all((yy - y) ** 2 + (xx - x) ** 2 >= sp * sp
               for dy in (-1, 0, 1) for dx in (-1, 0, 1) for (yy, xx) in grid.get((cy + dy, cx + dx), ())):
            grid.setdefault((cy, cx), []).append((y, x))
            keep.append((y, x))
    heat = np.zeros((hh, ww), np.float32)

    def stamp(y_lo, y_hi, x_lo, x_hi, fn):
        y_lo, y_hi, x_lo, x_hi = max(0, int(y_lo)), min(hh, int(y_hi)), max(0, int(x_lo)), min(ww, int(x_hi))
        if y_hi <= y_lo or x_hi <= x_lo:
            return
        Y, X = np.mgrid[y_lo:y_hi, x_lo:x_hi].astype(np.float32)
        region = heat[y_lo:y_hi, x_lo:x_hi]
        np.maximum(region, fn(Y, X), out=region)

    def tongue(y0, x0, dx, dy, L, Wd, bend):
        ts = np.linspace(0, 1, 14, dtype=np.float32)
        sx, sy = x0 + dx * L * ts, y0 + dy * L * ts - bend * L * ts ** 2
        r = Wd * (1 - ts) ** 0.75 + 0.35 * u
        def fn(Y, X):
            best = np.zeros(Y.shape, np.float32)
            for i in range(len(ts)):
                v = np.clip((r[i] - np.hypot(X - sx[i], Y - sy[i])) / (0.7 * u), 0, 1) * (1 - ts[i]) ** p['fade']
                np.maximum(best, v, out=best)
            return best
        stamp(sy.min() - Wd - 3, sy.max() + Wd + 3, sx.min() - Wd - 3, sx.max() + Wd + 3, fn)

    def blob(y0, x0, R, strength):
        stamp(y0 - R - 3, y0 + R + 3, x0 - R - 3, x0 + R + 3,
              lambda Y, X: np.clip((R - np.hypot(X - x0, Y - y0)) / (0.8 * u), 0, 1) * strength)

    for y, x in keep:
        ex, ey = nx[y, x], ny[y, x]
        rel = (y - top) / max(bot - top, 1)
        facing_up, facing_down, side = max(0.0, -ey), max(0.0, ey), abs(ex)
        low = float(np.clip((rel - 0.35) / 0.5, 0, 1))
        # Flame butts: clumps hugging the bottom and the lower sides.
        if low > 0 or facing_down > 0.4:
            k = max(low, facing_down)
            for _ in range(2):
                R = p['butt'] * u * (0.5 + 0.7 * rng.random()) * (0.5 + 0.6 * k)
                off = R * (0.2 + 0.4 * rng.random())
                blob(y + ey * off + (rng.random() - 0.5) * R, x + ex * off + (rng.random() - 0.5) * R, R, 0.75 + 0.25 * rng.random())
        # Tongues from every point that does not face down, out and up, curling upward.
        if facing_down < 0.55:
            d = np.array([ex * p['out'], ey * p['out'] - 1.0])
            d /= np.linalg.norm(d)
            L = (p['L0'] + p['Lup'] * facing_up ** 1.5 + p['Lside'] * side * low) * u * (0.7 + 0.6 * rng.random())
            Wd = p['W'] * u * (0.8 + 0.4 * rng.random()) * (1 + 0.5 * low)
            tongue(y - ey * 0.5 * u, x - ex * 0.5 * u, d[0], d[1], L, Wd, p['bend'] * side)
    # Turbulence: pushed about by noise (wispy edges), eaten by streaky noise toward the tips.
    wx = (fbm_n((hh, ww), rng, [4 * u, 2 * u], [1, 0.5], stretch=(2.0, 1)) - 0.5) * p['warp'] * u
    wy = (fbm_n((hh, ww), rng, [4 * u, 2 * u], [1, 0.5], stretch=(2.0, 1)) - 0.5) * p['warp'] * u * 1.6
    Y, X = np.mgrid[0:hh, 0:ww].astype(np.float32)
    heat = nd.map_coordinates(heat, [Y + wy, X + wx], order=1, mode='constant')
    feather = fbm_n((hh, ww), rng, [2.5 * u, 1.2 * u, 0.6 * u], [1, 0.7, 0.5], stretch=(2.6, 1))
    heat = heat * np.clip((feather - (1 - heat) * p['ragged'] + 0.2) / 0.3, 0, 1)
    heat = nd.gaussian_filter(heat, p['soft'] * u)
    heat = np.maximum(heat, np.exp(-dist / p['band']) * (~mask) * p['band_heat'])  # the hot band on the outline
    fire_rgb = ramp(heat, [(0.0, (45, 5, 0)), (0.3, (125, 20, 0)), (0.55, (200, 55, 5)), (0.8, (238, 112, 18)),
                           (1.0, (252, 165, 45))])
    fire_a = np.clip((heat - 0.04) / 0.55, 0, 1) ** 1.2 * p['alpha']
    rim = np.exp(-(np.maximum(dist - 0.3 * p['rim'], 0) / (0.6 * p['rim'])) ** 2)
    rim = np.where(mask, np.clip(1 - inside / (0.5 * p['rim']), 0, 1), rim)
    glow = (0.5 * np.exp(-dist / 2.5) + 0.5 * np.exp(-dist / p['glow'])) * p['glow_a']
    glow[mask] = 0
    rgb = np.array((255, 140, 40), np.float32)[None, None] * glow[..., None]
    al = glow.copy()
    for c, ca in ((fire_rgb, fire_a), (np.broadcast_to(np.array(p['rim_rgb'], np.float32), fire_rgb.shape), rim * p['rim_a'])):
        rgb = c * ca[..., None] + rgb * (1 - ca[..., None])
        al = ca + al * (1 - ca)
    col = np.where(al[..., None] > 1e-4, rgb / np.maximum(al[..., None], 1e-4), 0)
    al *= np.where(mask, np.clip(1 - inside / p['rim'], 0, 1), 1)  # the pad covers the rest of its inside
    return Image.fromarray(np.dstack([np.clip(col, 0, 255), np.clip(al * 255, 0, 255)]).astype(np.uint8), 'RGBA'), M / scale


def soften(render, radius):
    """A light Gaussian blur (premultiplied, so the transparent edge does not darken)."""
    a = np.asarray(render).astype(np.float32) / 255
    pm = np.dstack([a[..., :3] * a[..., 3:], a[..., 3:]])
    pm = np.dstack([nd.gaussian_filter(pm[..., c], radius) for c in range(4)])
    al = pm[..., 3:]
    rgb = np.where(al > 1e-4, pm[..., :3] / np.maximum(al, 1e-4), 0)
    return Image.fromarray((np.dstack([rgb, al]) * 255 + 0.5).clip(0, 255).astype(np.uint8), 'RGBA')


def silhouette(render):
    """Per band (BANDS from the top): the silhouette's left and right ends as fractions of the width (-1 -1: empty)."""
    a = np.asarray(render)[..., 3] > 128
    H, W = a.shape
    out = []
    for b in range(BANDS):
        rows = a[b * H // BANDS:(b + 1) * H // BANDS]
        cols = np.where(rows.any(axis=0))[0]
        out.append((cols[0] / W, (cols[-1] + 1) / W) if len(cols) else (-1.0, -1.0))
    return out


def hexcol(c):
    return np.array([int(c[i:i + 2], 16) for i in (0, 2, 4)], np.float32)


def brush(lit, taper, seed, L=512, T=24):
    """A grungy brushed line (L along, T across): a wobbling core with ragged edges and scratched-out streaks, its detail
    coarse enough along the stroke to survive being drawn a few times shorter than it is (no mipmaps).
    taper: 'start' narrows to a point at u 0 (a leader's end at the button), 'end' thins a little at u 1 (an underscore's
    outer end). Normal: the original's amber (core B68304 / BC8703, darker 7A590C patches, a brown lower edge 5B441E);
    lit: the bracket's yellow core with a cream top and rust edges."""
    rng = np.random.default_rng(seed)
    u = np.arange(L, dtype=np.float32)
    def noise1(scale, amp):
        g = rng.standard_normal(int(L / scale) + 4).astype(np.float32)
        return nd.zoom(g, (L + 3 * scale) / len(g), order=3)[:L] * amp
    t = u / (L - 1)
    centre = T / 2 + noise1(140, 0.8) + noise1(30, 0.45)
    half = 5.4 + noise1(80, 0.6) + noise1(18, 0.4)
    if taper == 'start':
        half *= np.clip(t / 0.4, 0.05, 1) ** 0.8
    elif taper == 'end':
        half *= np.clip((1 - t) / 0.06, 0.45, 1)
    y = np.arange(T, dtype=np.float32)[:, None] + 0.5
    # Ragged edges: each edge pushed in and out on its own.
    top = centre - half + noise1(12, 0.55) + noise1(6, 0.35)
    bottom = centre + half + noise1(12, 0.55) + noise1(6, 0.35)
    cover = np.clip(np.minimum(y - top[None, :], bottom[None, :] - y) + 0.5, 0, 1)
    d = (y - centre[None, :]) / np.maximum(half[None, :], 0.6)  # -1 top .. 1 bottom
    # Scratches: streaks along the stroke where the paint is thin.
    streak = np.zeros((T, L), np.float32)
    for scale, amp in ((60, 1.0), (24, 0.55)):
        g = rng.random((int(T / 2) + 3, int(L / scale) + 3)).astype(np.float32)
        z = nd.zoom(g, ((T + 4) / g.shape[0], (L + 2 * scale) / g.shape[1]), order=1)[:T, :L]
        streak += amp * z
    streak /= 1.55
    scratch = np.clip((streak - 0.6) / 0.12, 0, 1) * 0.7
    alpha = cover * (1 - scratch)
    shade = np.clip(0.62 + noise1(50, 0.3) + noise1(14, 0.18), 0, 1)
    if lit:
        core, topc, edge = hexcol('FFFE11'), hexcol('FFFEC4'), hexcol('B84F25')
        rgb = np.where((d < -0.35)[..., None], topc, core)
        rgb = np.where((np.abs(d) > 0.72)[..., None], edge, rgb)
    else:
        bright, dark, edge = hexcol('C48C05'), hexcol('7A590C'), hexcol('5B441E')
        mix = np.clip(shade[None, :] + 0.3 * (1 - np.abs(d)) - 0.4 * scratch, 0, 1)[..., None]
        rgb = dark * (1 - mix) + bright * mix
        rgb = np.where((d > 0.6)[..., None], edge, rgb)
    return Image.fromarray(np.dstack([np.clip(rgb, 0, 255), alpha * 255]).astype(np.uint8), 'RGBA')


def panel():
    """The label panels' wash: white, alpha 1 at the outer end (u 0) fading to nothing at the inner end (K: no edge by
    the pad), tinted by the overlay (options_overlay.cpp kMapPanel)."""
    t = np.linspace(0, 1, 256, dtype=np.float32)
    a = np.tile(np.clip(1.0 - t, 0, 1)[None, :] ** 1.2, (4, 1))
    return Image.fromarray(np.dstack([np.full_like(a, 255), np.full_like(a, 255), np.full_like(a, 255), a * 255]).astype(np.uint8), 'RGBA')


def stick_without_arrow(path):
    """prompt_hd.cpp's "#noarrow": the click arrow (the first run of visible rows from the top) cleared, the frame kept."""
    im = np.asarray(Image.open(path).convert('RGBA')).copy()
    visible = im[..., 3] > 4
    rows = np.where(visible.any(axis=1))[0]
    if len(rows):
        y = rows[0]
        while y < im.shape[0] and visible[y].any():
            y += 1
        if y < im.shape[0] and visible[y:].any():
            im[:y] = 0
    return Image.fromarray(im, 'RGBA')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('renders')
    ap.add_argument('--seed', type=int, default=5)
    ap.add_argument('--out', default=os.path.join(ROOT, 'resources/options/mapping'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name, pad in PADS.items():
        src = os.path.join(a.renders, 'pad_%s.png' % name)
        render = Image.open(src).convert('RGBA')
        if name == 'xbox':
            soften(render, XBOX_BLUR).save(os.path.join(a.out, 'pad_%s.png' % name), optimize=True)
        else:
            shutil.copyfile(src, os.path.join(a.out, 'pad_%s.png' % name))
        fl, margin = flames(render, a.seed)
        fl.save(os.path.join(a.out, 'flames_%s.png' % name), optimize=True)
        lines = ['# The Button Mapping page\'s %s pad: written by tools/options/make_pad_art.py (edit the points there).' % name,
                 'image pad_%s.png %d %d' % (name, render.width, render.height),
                 'flames flames_%s.png %.1f' % (name, margin),
                 '# point <button> <x> <y> <how far short of it the line stops>, in the render\'s pixels']
        for button, (x, y, r) in pad['points'].items():
            lines.append('point %s %d %d %d' % (button, x, y, r))
        for button, (x, y) in pad.get('via', {}).items():
            lines.append('via %s %d %d' % (button, x, y))
        lines.append('left ' + ' '.join(pad['left']))
        lines.append('right ' + ' '.join(pad['right']))
        lines.append('# the silhouette\'s left and right ends per band from the top (fractions of the width; -1: none)')
        lines.append('silhouette %d %s' % (BANDS, ' '.join('%.4f %.4f' % p for p in silhouette(render))))
        open(os.path.join(a.out, 'pad_%s.txt' % name), 'w', newline='\n').write('\n'.join(lines) + '\n')
        print(name, 'flames margin', margin)
    for lit in (False, True):
        suffix = '_lit' if lit else ''
        brush(lit, 'start', 3).save(os.path.join(a.out, 'leader%s.png' % suffix))
        brush(lit, 'end', 4).save(os.path.join(a.out, 'underscore%s.png' % suffix))
    panel().save(os.path.join(a.out, 'panel.png'))
    for folder, prefix in STICK_SETS.items():
        for side, file in (('left', 'Left_Stick_Click'), ('right', 'Right_Stick_Click')):
            p = os.path.join(ROOT, 'resources/prompts/xelu', folder, '%s_%s.png' % (prefix, file))
            stick_without_arrow(p).save(os.path.join(a.out, 'stick_%s_%s.png' % (folder, side)))


if __name__ == '__main__':
    main()
