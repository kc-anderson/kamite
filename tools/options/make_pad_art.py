"""The Button Mapping page's art (CONTROLS > BUTTON MAPPING), in the look of the game's own controller page
(claude/button-mapping.md "The original controller page"). Our own drawings from measured colours and K's pad renders:
nothing of the game's art ships.

    python tools/options/make_pad_art.py RENDERS_DIR [--flames A|B|C] [--seed N] [--out DIR]

RENDERS_DIR holds K's renders (pad_xbox.png, pad_playstation.png, pad_nintendo.png: front views on transparent
backgrounds). Writes into resources/options/mapping (or --out):
- pad_<name>.png: the render as it is; flames_<name>.png: the flame outline round its silhouette (variant --flames;
  half the render's resolution, MARGIN of its height round it);
- pad_<name>.txt: the page's data for it (mapping_page.cpp loadArt): the images, each button's point, the columns'
  rows top to bottom and the silhouette's ends per band (the label panels stop just short of it, as the original's do);
- brush.png, brush_lit.png: the lines' stroke (stretched along each line), normal and selected;
- panel.png: the label panels' wash (opaque at the outer end, a quarter at the inner);
- stick_<set>_<left|right>.png: the pack's stick-press icons without the click arrow (K, Nov 3: the stick icons are the
  click images' side view, same frame), as prompt_hd.cpp makes them.
"""
import argparse, os, shutil
import numpy as np
from PIL import Image
from scipy import ndimage as nd

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MARGIN = 0.14   # the flames' room round the render (of its height)
BANDS = 64      # silhouette bands

# Each pad: its buttons (render pixels: x, y, how far short of the point its line stops) and the columns' rows. A row
# shows only when it has something to say (mapping_page.cpp); the order keeps the lines from crossing. A stick's click
# line ends on its cap, its own line on the ring below, so the two never run together.
PADS = {
    'xbox': dict(
        points=dict(south=(1128, 535, 42), east=(1238, 432, 42), west=(1018, 432, 42), north=(1130, 330, 42),
                    l1=(300, 200, 0), r1=(1150, 200, 0), l2=(345, 68, 0), r2=(1110, 68, 0),
                    l3=(310, 412, 18), r3=(918, 637, 18), start=(880, 428, 26), select=(565, 430, 26),
                    dpad=(500, 665, 70), lstick=(310, 478, 62), rstick=(918, 703, 62)),
        left=['l2', 'l1', 'l3', 'lstick', 'dpad'],
        right=['start', 'r2', 'r1', 'north', 'east', 'south', 'west', 'r3', 'rstick']),
    'playstation': dict(
        points=dict(south=(1265, 563, 48), east=(1390, 440, 48), west=(1140, 440, 48), north=(1268, 318, 48),
                    l1=(250, 140, 0), r1=(1335, 140, 0), l2=(255, 45, 0), r2=(1330, 45, 0),
                    l3=(552, 640, 18), r3=(1027, 640, 18), start=(935, 432, 30), select=(648, 432, 30),
                    dpad=(322, 438, 100), lstick=(552, 706, 62), rstick=(1027, 706, 62)),
        left=['l2', 'l1', 'dpad', 'l3', 'lstick'],
        right=['start', 'r2', 'r1', 'north', 'east', 'south', 'west', 'r3', 'rstick']),
    'nintendo': dict(
        points=dict(south=(1193, 515, 42), east=(1313, 422, 42), west=(1067, 422, 42), north=(1190, 328, 42),
                    l1=(310, 110, 0), r1=(1215, 110, 0), l2=(340, 38, 0), r2=(1190, 38, 0),
                    l3=(530, 615, 16), r3=(995, 615, 16), start=(865, 415, 25), select=(670, 415, 25),
                    dpad=(338, 418, 85), lstick=(530, 672, 55), rstick=(995, 672, 55)),
        left=['l2', 'l1', 'dpad', 'l3', 'lstick'],
        right=['start', 'r2', 'r1', 'north', 'east', 'south', 'west', 'r3', 'rstick']),
}

# Flame variants (K picks): reach in lines of the page (the pad drawn ~232 lines tall), the upward extra, the soft glow.
FLAMES = {
    'A': dict(base=4, up=9, side=2, flame_alpha=0.9, glow=9, glow_alpha=0.28, rim=1.1, gap=0.45, floor=0.25, cool=1.3),
    'B': dict(base=8, up=26, side=3, flame_alpha=0.95, glow=15, glow_alpha=0.34, rim=1.1, gap=0.42, floor=0.3, cool=1.2),
    'C': dict(base=13, up=42, side=5, flame_alpha=1.0, glow=20, glow_alpha=0.42, rim=1.2, gap=0.36, floor=0.4, cool=0.95),
}

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


def fire_ramp(t):
    """Heat 1 (at the outline) .. 0 (the tips): pale yellow, yellow, orange, red, dark red."""
    stops = [(0.0, (110, 10, 0)), (0.25, (190, 40, 0)), (0.5, (245, 110, 10)), (0.75, (255, 180, 40)), (1.0, (255, 235, 150))]
    t = np.clip(t, 0, 1)
    out = np.zeros(t.shape + (3,), np.float32)
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        m = (t >= t0) & (t <= t1)
        f = ((t - t0) / (t1 - t0))[m][:, None]
        out[m] = np.array(c0) * (1 - f) + np.array(c1) * f
    return out


def flames(render, v, seed):
    """The flame outline (RGBA, half the render's resolution, MARGIN of its height round it): a thin pale rim on the
    outline, tongues licking out (longest where the outline faces up), a soft orange glow beyond."""
    W, H = render.size
    scale = 0.5
    w, h = int(W * scale), int(H * scale)
    a = np.asarray(render.resize((w, h), Image.LANCZOS))[..., 3].astype(np.float32) / 255
    M = int(round(MARGIN * H * scale))
    a = np.pad(a, M)
    mask = a > 0.5
    hh, ww = a.shape
    dist = nd.distance_transform_edt(~mask).astype(np.float32)
    inside = nd.distance_transform_edt(mask).astype(np.float32)
    gy, gx = np.gradient(nd.gaussian_filter(dist, 6))
    n = np.hypot(gx, gy) + 1e-6
    up, side = np.clip(-gy / n, 0, 1), np.abs(gx / n)
    rng = np.random.default_rng(seed)
    unit = H * scale / 232.0
    lick = fbm((hh, ww), rng, [6 * unit, 3 * unit, 1.5 * unit], [1.0, 0.6, 0.35], stretch=(2.6, 1.0))
    coarse = fbm((hh, ww), rng, [40 * unit, 20 * unit], [1.0, 0.5])
    reach = (v['base'] + v['up'] * up ** 1.5 + v['side'] * side * 0.3) * unit * (0.55 + 0.9 * coarse)
    f = dist / np.maximum(reach, 1e-3)
    tongue = np.clip((lick - v['gap']) / 0.35, 0, 1)
    heat = np.clip(1.0 - f * (1.3 - 0.75 * tongue), 0, 1) * np.clip(v['floor'] + (1 - v['floor']) * tongue, 0, 1)
    heat = nd.gaussian_filter(heat, 0.6 * unit)
    rgb = fire_ramp(heat ** v['cool'])
    alpha = np.clip(heat * 1.6, 0, 1) ** 1.2 * v['flame_alpha']
    glow = np.exp(-dist / (v['glow'] * unit)) * v['glow_alpha']
    rim_w = v['rim'] * unit
    rim = np.clip(1.0 - np.maximum(dist - 0.5 * rim_w, 0) / (0.6 * rim_w), 0, 1)
    rim = np.where(mask, np.clip(1.0 - inside / (0.6 * rim_w), 0, 1) * 0.9, rim)
    out_rgb = np.array((255, 120, 20), np.float32)[None, None] * glow[..., None]
    out_a = glow.copy()
    for c_rgb, c_a in ((rgb, alpha), (np.broadcast_to(np.array((255, 243, 190), np.float32), rgb.shape), rim)):
        out_rgb = c_rgb * c_a[..., None] + out_rgb * (1 - c_a[..., None])
        out_a = c_a + out_a * (1 - c_a)
    col = np.where(out_a[..., None] > 1e-4, out_rgb / np.maximum(out_a[..., None], 1e-4), 0)
    out_a *= np.where(mask, np.clip(1.0 - inside / rim_w, 0, 1), 1.0)  # the pad covers the rest of its inside
    return Image.fromarray(np.dstack([np.clip(col, 0, 255), np.clip(out_a * 255, 0, 255)]).astype(np.uint8), 'RGBA'), M / scale


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


def brush(lit, seed=3, L=1024, T=16):
    """A brushed line (L along, T across): a wobbling core with blunt ends; normal = the original's amber (core B68304 /
    BC8703, darker 7A590C, a brown lower edge 5B441E), lit = the bracket's yellow core with a cream top and rust edges."""
    rng = np.random.default_rng(seed)
    u = np.arange(L, dtype=np.float32)
    def noise1(scale, amp):
        g = rng.standard_normal(int(L / scale) + 4).astype(np.float32)
        return nd.zoom(g, (L + 3 * scale) / len(g), order=3)[:L] * amp
    centre = T / 2 + noise1(90, 0.8) + noise1(14, 0.4)
    half = 3.5 + noise1(60, 0.5) + noise1(9, 0.3)
    shade = np.clip(0.68 + noise1(40, 0.3) + noise1(7, 0.18), 0, 1)
    y = np.arange(T, dtype=np.float32)[:, None] + 0.5
    d = (y - centre[None, :]) / np.maximum(half[None, :], 0.8)  # -1 top edge .. 1 bottom edge
    cover = np.clip((1.0 - np.abs(d)) * half[None, :] + 0.5, 0, 1)  # anti-aliased edges
    if lit:
        core, top, edge = hexcol('FFFE11'), hexcol('FFFEC4'), hexcol('B84F25')
        rgb = np.where((d < -0.35)[..., None], top, core)
        rgb = np.where((np.abs(d) > 0.7)[..., None], edge, rgb)
    else:
        bright, dark, edge = hexcol('BC8703'), hexcol('7A590C'), hexcol('5B441E')
        mix = np.clip(shade[None, :] + 0.25 * (1 - np.abs(d)), 0, 1)[..., None]
        rgb = dark * (1 - mix) + bright * mix
        rgb = np.where((d > 0.55)[..., None], edge, rgb)
    return Image.fromarray(np.dstack([np.clip(rgb, 0, 255), cover * 255]).astype(np.uint8), 'RGBA')


def panel():
    """The label panels' wash: white, alpha from 1 at the outer end (u 0) to a quarter at the inner end, tinted by the
    overlay (options_overlay.cpp kPanelTint)."""
    t = np.linspace(0, 1, 256, dtype=np.float32)
    a = np.tile((1.0 - 0.75 * t)[None, :], (4, 1))
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
    ap.add_argument('--flames', choices=sorted(FLAMES), default='B')
    ap.add_argument('--seed', type=int, default=7)
    ap.add_argument('--out', default=os.path.join(ROOT, 'resources/options/mapping'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name, pad in PADS.items():
        src = os.path.join(a.renders, 'pad_%s.png' % name)
        render = Image.open(src).convert('RGBA')
        shutil.copyfile(src, os.path.join(a.out, 'pad_%s.png' % name))
        fl, margin = flames(render, FLAMES[a.flames], a.seed)
        fl.save(os.path.join(a.out, 'flames_%s.png' % name), optimize=True)
        lines = ['# The Button Mapping page\'s %s pad: written by tools/options/make_pad_art.py (edit the points there).' % name,
                 'image pad_%s.png %d %d' % (name, render.width, render.height),
                 'flames flames_%s.png %.1f' % (name, margin),
                 '# point <button> <x> <y> <how far short of it the line stops>, in the render\'s pixels']
        for button, (x, y, r) in pad['points'].items():
            lines.append('point %s %d %d %d' % (button, x, y, r))
        lines.append('left ' + ' '.join(pad['left']))
        lines.append('right ' + ' '.join(pad['right']))
        lines.append('# the silhouette\'s left and right ends per band from the top (fractions of the width; -1: none)')
        lines.append('silhouette %d %s' % (BANDS, ' '.join('%.4f %.4f' % p for p in silhouette(render))))
        open(os.path.join(a.out, 'pad_%s.txt' % name), 'w', newline='\n').write('\n'.join(lines) + '\n')
        print(name, 'flames margin', margin)
    brush(False).save(os.path.join(a.out, 'brush.png'))
    brush(True).save(os.path.join(a.out, 'brush_lit.png'))
    panel().save(os.path.join(a.out, 'panel.png'))
    for folder, prefix in STICK_SETS.items():
        for side, file in (('left', 'Left_Stick_Click'), ('right', 'Right_Stick_Click')):
            p = os.path.join(ROOT, 'resources/prompts/xelu', folder, '%s_%s.png' % (prefix, file))
            stick_without_arrow(p).save(os.path.join(a.out, 'stick_%s_%s.png' % (folder, side)))


if __name__ == '__main__':
    main()
