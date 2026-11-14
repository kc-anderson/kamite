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
MARGIN = 0.20   # the flames' room round the render (of its height)
BANDS = 64      # silhouette bands

# Each pad: its buttons (render pixels: x, y, how far short of the point its line stops). The columns' rows are the same
# for every pad (mapping_page.cpp kLeft / kRight): only the lines change, bending (via) where they would cross. A stick's click
# line ends on its cap, its own line on the ring below, so the two never run together. via: a bend the line takes on its
# way (the left face button's line passes under the bottom one, as the original's square line does; Select and Start go
# down between the d-pad / left stick and the right stick and out under the pad to the bottom rows, under the sticks'
# lines).
PADS = {
    'xbox': dict(
        points=dict(south=(1128, 535, 42), east=(1238, 432, 42), west=(1018, 432, 42), north=(1130, 330, 42),
                    l1=(255, 205, 0), r1=(1225, 212, 0), l2=(300, 112, 0), r2=(1182, 140, 0),
                    l3=(310, 412, 18), r3=(918, 637, 18), start=(880, 428, 26), select=(565, 430, 26),
                    dpad=(500, 665, 70), lstick=(310, 478, 62), rstick=(918, 703, 62)),
        via=dict(west=(1095, 612), dpad=(440, 300), select=(640, 790), start=(770, 800))),
    'playstation': dict(
        points=dict(south=(1265, 563, 48), east=(1390, 440, 48), west=(1140, 440, 48), north=(1268, 318, 48),
                    l1=(250, 140, 0), r1=(1335, 140, 0), l2=(255, 45, 0), r2=(1330, 45, 0),
                    l3=(552, 640, 18), r3=(1027, 640, 18), start=(935, 432, 30), select=(648, 432, 30),
                    dpad=(322, 438, 100), lstick=(552, 706, 62), rstick=(1027, 706, 62)),
        via=dict(west=(1212, 645), select=(760, 900), start=(830, 900))),
    'nintendo': dict(
        points=dict(south=(1193, 515, 42), east=(1313, 422, 42), west=(1067, 422, 42), north=(1190, 328, 42),
                    l1=(300, 110, 0), r1=(1255, 120, 0), l2=(318, 52, 0), r2=(1238, 64, 0),
                    l3=(530, 615, 16), r3=(995, 615, 16), start=(865, 415, 25), select=(670, 415, 25),
                    dpad=(338, 418, 85), lstick=(530, 672, 55), rstick=(995, 672, 55)),
        via=dict(west=(1150, 592), select=(730, 860), start=(800, 860))),
}

# The flames (K, Nov 14, from his upscale of the original): a fire behind the pad, seen past its outline: a thin band
# at the bottom growing into big flames over the top (reach Rbot..Rtop lines by the outline's height, Rup more where it
# faces up); flame texture from swirled, ridged noise stretched up and down (wispy, see-through, yellow cores, red
# edges); the wall scorched dark round it (char); a few sparks; a thin pale-yellow rim with a narrow orange glow.
FIRE = dict(Rbot=3.0, Rtop=19.0, Rup=7.0, rise=1.0, warp=6.0, sy=13.0, sx=4.5, fallpow=1.3, gain=1.3, cut=0.7,
            alpha_gain=1.3, ridge=0.6, char=0.45, char_reach=1.3, sparks=14, rim=1.3, rim_a=0.95,
            rim_rgb=(255, 222, 120), glow=1.8, glow_a=0.6)
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


def noise2(shape, rng, scale_y, scale_x):
    """Smooth value noise, one octave, its cells scale_y x scale_x pixels."""
    h, w = shape
    gh, gw = max(2, int(h / scale_y) + 3), max(2, int(w / scale_x) + 3)
    z = nd.zoom(rng.random((gh, gw)).astype(np.float32), ((h + 2 * scale_y) / gh, (w + 2 * scale_x) / gw), order=3)
    return z[:h, :w]


def fbm2(shape, rng, sy, sx, octaves=4, gain=0.55):
    """Octaves of noise2 halving in size, normalised to mean 0.5, about a sixth spread."""
    out, amp, total = np.zeros(shape, np.float32), 1.0, 0.0
    for o in range(octaves):
        out += amp * noise2(shape, rng, max(sy / 2 ** o, 1.5), max(sx / 2 ** o, 1.5))
        total += amp
        amp *= gain
    out /= total
    return (out - out.mean()) / (out.std() * 3.0) + 0.5


def flames(render, seed):
    """The flame outline (RGBA, half the render's resolution, MARGIN of its height round it): scorch, glow, fire,
    sparks and rim, composited (FIRE)."""
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
    rng = np.random.default_rng(seed)
    dist, idx = nd.distance_transform_edt(~mask, return_indices=True)
    dist = dist.astype(np.float32) / u
    inside = nd.distance_transform_edt(mask).astype(np.float32) / u
    rows = np.where(mask.any(axis=1))[0]
    top, bot = rows[0], rows[-1]
    rel = np.clip((idx[0].astype(np.float32) - top) / max(bot - top, 1), 0, 1)  # the nearest outline point's height
    gy, gx = np.gradient(nd.gaussian_filter(dist - inside, 3 * u))
    up = np.clip(-gy / (np.hypot(gx, gy) + 1e-6), 0, 1)
    reach = (p['Rbot'] + (p['Rtop'] - p['Rbot']) * (1 - rel) ** p['rise'] + p['Rup'] * up) * u
    Y, X = np.mgrid[0:hh, 0:ww].astype(np.float32)
    wx = (fbm2((hh, ww), rng, 14 * u, 10 * u) - 0.5) * p['warp'] * u
    wy = (fbm2((hh, ww), rng, 14 * u, 10 * u) - 0.5) * p['warp'] * u
    base = nd.map_coordinates(fbm2((hh, ww), rng, p['sy'] * u, p['sx'] * u, octaves=5), [Y + wy, X + wx], order=1, mode='reflect')
    flame = (1 - p['ridge']) * base + p['ridge'] * (1.0 - np.abs(2.0 * base - 1.0)) ** 2
    fall = np.clip(1.0 - dist / np.maximum(reach, 1e-3), 0, 1)
    heat = np.clip(flame * fall ** p['fallpow'] * p['gain'] - (1 - fall) * p['cut'], 0, 1)
    heat[mask] = 0
    heat = nd.gaussian_filter(heat, 0.4 * u)
    fire_rgb = ramp(heat, [(0.0, (70, 8, 0)), (0.25, (170, 35, 0)), (0.5, (235, 95, 10)), (0.75, (250, 160, 35)),
                           (1.0, (255, 225, 120))])
    fire_a = np.clip(heat * p['alpha_gain'], 0, 1) ** 0.9
    char = np.clip(1 - dist / (reach * p['char_reach'] + 6 * u), 0, 1) ** 0.8 * \
        (0.55 + 0.6 * (fbm2((hh, ww), rng, 8 * u, 8 * u) - 0.5)) * p['char']
    char[mask] = 0
    sparks = np.zeros((hh, ww), np.float32)
    cand = np.argwhere((fall > 0.05) & (fall < 0.6) & (~mask))
    for k in rng.choice(len(cand), size=min(int(p['sparks']), len(cand)), replace=False):
        y, x = cand[k]
        r = (0.35 + 0.5 * rng.random()) * u
        y0, y1 = int(max(0, y - 3 * r - 2)), int(min(hh, y + 3 * r + 2))
        x0, x1 = int(max(0, x - 3 * r - 2)), int(min(ww, x + 3 * r + 2))
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        v = np.exp(-((yy - y) ** 2 + (xx - x) ** 2) / (2 * r * r)) * (0.6 + 0.4 * rng.random())
        np.maximum(sparks[y0:y1, x0:x1], v, out=sparks[y0:y1, x0:x1])
    rim = np.exp(-(np.maximum(dist - 0.2 * p['rim'], 0) / (0.55 * p['rim'])) ** 2)
    rim = np.where(mask, np.clip(1 - inside / (0.5 * p['rim']), 0, 1), rim)
    glow = np.exp(-dist / p['glow']) * p['glow_a']
    glow[mask] = 0
    rgb, al = np.zeros((hh, ww, 3), np.float32), np.zeros((hh, ww), np.float32)
    for c, ca in ((np.array((22, 10, 4), np.float32), char), (np.array((255, 130, 30), np.float32), glow), (fire_rgb, fire_a),
                  (np.array((255, 190, 70), np.float32), sparks), (np.array(p['rim_rgb'], np.float32), rim * p['rim_a'])):
        c = np.broadcast_to(c, rgb.shape)
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
