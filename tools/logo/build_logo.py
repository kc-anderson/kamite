"""Builds the launcher's title art from two source images:

  wordmark: the KAMITE wordmark render (transparent PNG)
  kana:     the brush hiragana "kamite" (black or any colour on transparent; only alpha is used)

Writes into resources/images/:
  logo.png   the KAMITE wordmark alone, on the whole canvas (room above it where the kana go); the launcher
             keeps it still
  kana.png   the painted turquoise kana with their shadow and splatter, cropped; the launcher draws it over
             logo.png at the rectangle this script prints (kKanaRect in src/kamite/launcher.cpp) and
             shifts it with the mouse
  burst.png  the speed-line burst the launcher draws behind it (turquoise, transparent; the launcher
             rotates, pulses and parallax-shifts it)
and, with --psd, a layered PSD of the logo (wordmark, kana shadow, kana paint, splatter) for hand edits.

The source images are not in the repo; pass their paths:
  python tools/logo/build_logo.py --wordmark KAMITE.png --kana kamite_kana.png [--psd logo.psd]
or rebuild the PNGs from a layered PSD written earlier (hand-edited or not):
  python tools/logo/build_logo.py --layers logo.psd

Needs numpy, scipy, pillow (and psd-tools for --psd / --layers).
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage as ndi

F = np.float32
TURQUOISE = (0x2C, 0xC9, 0xB9)

# Where the kana sit relative to the wordmark (fractions of the wordmark image's width/height,
# measured from its centre), their width and their tilt (degrees, negative = clockwise).
KANA_CENTRE = (0.18, -0.62)
KANA_WIDTH = 0.50
KANA_TILT = -5.0

# The paint: "Subtle" paint with "Medium" splatter and no dry-brush breakup, so the edges stay solid.
PAINT = dict(tone=0.6, sheen=0.20, drops=45, fling=55, drop_r=3.0, mist=0.003, spl_alpha=0.92)
REF_KANA_WIDTH = 947.0  # the kana width the paint was tuned at; sizes scale from it


def fbm(shape, cell, rng, octaves=4):
    acc = np.zeros(shape, F)
    amp = 1.0
    for _ in range(octaves):
        c = max(2, int(cell))
        g = rng.standard_normal((shape[0] // c + 4, shape[1] // c + 4)).astype(F)
        acc += amp * ndi.zoom(g, c, order=3, prefilter=False)[:shape[0], :shape[1]]
        amp *= 0.5
        cell /= 2
    return (acc - acc.mean()) / (acc.std() + 1e-6)


def painted_kana(mask, s, seed=1):
    """mask: float 0..1 kana alpha. Returns (rgb, paint alpha, splatter alpha, pad) on a padded canvas."""
    p = PAINT
    rng = np.random.default_rng(seed)
    pad = int(160 * s)
    m = np.pad(mask, pad)
    shape = m.shape
    inside = m > 0.5
    d_in = ndi.distance_transform_edt(inside).astype(F)

    # Bristle tracks that follow the stroke (indexed by the distance from its edge, wobbled).
    wob = fbm(shape, 60 * s, rng, 3)
    track = (d_in + 2.2 * s * wob) * (1.3 / s)
    lut = ndi.gaussian_filter1d(rng.standard_normal(8192).astype(F), 0.9)
    lut /= lut.std()
    bristle = np.interp(track, np.arange(8192), lut).astype(F)
    along = fbm(shape, 70 * s, rng, 3)
    streak = bristle * (0.55 + 0.45 * np.tanh(along))
    fine = fbm(shape, 10 * s, rng, 3)
    grain = fbm(shape, 3 * s, rng, 2)

    # Colour: thin paint a little lighter, plus a soft wet sheen from the stroke's relief.
    base = np.array(TURQUOISE, F) / 255
    dens = np.clip(0.5 + 0.16 * streak + 0.08 * fine + 0.04 * grain + 0.12 * along, 0, 1)
    rgb = base[None, None, :] * (0.82 + 0.36 * (1 - dens) * p["tone"])[..., None]
    hgt = ndi.gaussian_filter(np.minimum(d_in, 18 * s) / s + 1.2 * streak, 1.5 * s)
    gy, gx = np.gradient(hgt)
    gy, gx = gy * s, gx * s
    sheen = np.clip(-(gx * 0.6 + gy * 0.8), 0, None) ** 1.5
    rgb = np.clip(rgb + (p["sheen"] * np.clip(sheen, 0, 1))[..., None] * np.array([0.8, 1.0, 1.0], F), 0, 1)

    # Splatter: drops flung mostly from the stroke ends, with tails and satellite specks, plus a fine mist.
    spl = Image.new("L", (shape[1], shape[0]), 0)
    dr = ImageDraw.Draw(spl)
    ys, xs = np.nonzero(inside & (d_in < 6 * s))
    ends = ndi.uniform_filter(inside.astype(F), int(61 * s) | 1)
    w_end = (1 - ends[ys, xs]) ** 2
    w_end /= w_end.sum()
    cy, cx = np.mean(np.nonzero(inside), axis=1)
    fling, drop_r = p["fling"] * s, p["drop_r"] * s
    for i in rng.choice(len(ys), size=p["drops"], p=w_end):
        y0, x0 = ys[i], xs[i]
        v = np.array([y0 - cy, x0 - cx], F)
        v /= np.linalg.norm(v) + 1e-6
        v = v * 0.6 + rng.normal(0, 0.5, 2)
        v /= np.linalg.norm(v) + 1e-6
        dist = rng.gamma(1.6, fling)
        rad = min(max(s, (rng.pareto(2.2) + 0.6) * drop_r), drop_r * 6)
        py, px = y0 + v[0] * dist, x0 + v[1] * dist
        stretch = 1 + min(2.5, dist / (fling * 2))
        steps = int(stretch * 3 * max(1.0, s))
        for k in range(steps):
            t = k / max(1, steps - 1)
            rr = rad * (1 - 0.6 * t)
            qy, qx = py - v[0] * rad * stretch * t, px - v[1] * rad * stretch * t
            dr.ellipse((qx - rr, qy - rr, qx + rr, qy + rr), fill=255)
        for _ in range(rng.integers(0, 4)):
            sr = max(0.8 * s, rad * rng.uniform(0.15, 0.35))
            sy, sx = py + rng.normal(0, rad * 2.2), px + rng.normal(0, rad * 2.2)
            dr.ellipse((sx - sr, sy - sr, sx + sr, sy + sr), fill=255)
    near = (ndi.distance_transform_edt(~inside) < fling * 0.8) & ~inside
    mist = near & (rng.random(shape) < p["mist"] / (s * s))
    mist = ndi.binary_dilation(mist, iterations=max(1, round(s)))
    spl_a = np.maximum(np.asarray(spl, F) / 255, mist.astype(F) * 0.9)
    spl_a = ndi.gaussian_filter(spl_a, 0.6 * s) * p["spl_alpha"] * (1 - inside)
    return rgb, m, np.clip(spl_a, 0, 1), pad


def rgba(rgb, a):
    return Image.fromarray((np.dstack([np.clip(rgb, 0, 1), np.clip(a, 0, 1)]) * 255 + 0.5).astype(np.uint8), "RGBA")


def bleed(img, passes=None):
    """Spread the colour of visible pixels into the fully transparent ones, so filtered/mipmapped
    drawing does not pull in stray colours at the edges."""
    arr = np.asarray(img).copy()
    a = arr[..., 3] > 0
    if a.all() or not a.any():
        return img
    _, (iy, ix) = ndi.distance_transform_edt(~a, return_indices=True)
    arr[..., :3] = np.where(a[..., None], arr[..., :3], arr[iy, ix, :3])
    return Image.fromarray(arr, "RGBA")


def build_logo(wordmark_path, kana_path, seed=1):
    word = Image.open(wordmark_path).convert("RGBA")
    ww, wh = word.size
    kana_a = np.asarray(Image.open(kana_path).convert("RGBA"))[..., 3]
    kana_img = Image.fromarray(kana_a, "L")
    kana_img = kana_img.crop(kana_img.getbbox())
    kw = int(ww * KANA_WIDTH)
    kana_img = kana_img.resize((kw, round(kana_img.height * kw / kana_img.width)), Image.LANCZOS)
    s = kw / REF_KANA_WIDTH

    rgb, paint_a, spl_a, pad = painted_kana(np.asarray(kana_img, F) / 255, s, seed)
    paint = rgba(rgb, paint_a)
    splat = rgba(np.broadcast_to(np.array(TURQUOISE, F) / 255, rgb.shape), spl_a)
    # Shadow and a thin dark edge under paint + splatter, so they read over the busy wordmark.
    both_a = Image.fromarray((np.maximum(paint_a, spl_a) * 255).astype(np.uint8), "L")
    shadow = Image.new("RGBA", both_a.size, (0, 0, 0, 0))
    sh = Image.new("RGBA", both_a.size, (0, 0, 0, 0))
    sh.putalpha(both_a.filter(ImageFilter.GaussianBlur(6 * s)).point(lambda v: int(v * 0.75)))
    shadow.alpha_composite(sh, (round(6 * s), round(8 * s)))
    edge = Image.new("RGBA", both_a.size, (8, 8, 10, 0))
    edge.putalpha(both_a.filter(ImageFilter.MaxFilter(2 * round(2 * s) + 1)).point(lambda v: int(v * 0.85)))
    shadow.alpha_composite(edge)

    # Canvas: the wordmark with room above and to the sides for the kana and the splatter.
    rot = [im.rotate(KANA_TILT, resample=Image.BICUBIC, expand=True) for im in (shadow, paint, splat)]
    kcx = ww / 2 + KANA_CENTRE[0] * ww
    kcy = wh / 2 + KANA_CENTRE[1] * wh
    left = min(0, int(kcx - rot[0].width / 2))
    top = min(0, int(kcy - rot[0].height / 2))
    right = max(ww, int(kcx + rot[0].width / 2) + 1)
    bottom = max(wh, int(kcy + rot[0].height / 2) + 1)
    size = (right - left, bottom - top)
    layers = {}
    layers["wordmark"] = Image.new("RGBA", size, (0, 0, 0, 0))
    layers["wordmark"].alpha_composite(word, (-left, -top))
    for name, im in zip(("kana shadow", "kana paint", "splatter"), rot):
        layer = Image.new("RGBA", size, (0, 0, 0, 0))
        layer.alpha_composite(im, (round(kcx - im.width / 2) - left, round(kcy - im.height / 2) - top))
        layers[name] = layer
    flat = Image.new("RGBA", size, (0, 0, 0, 0))
    for name in ("wordmark", "kana shadow", "kana paint", "splatter"):
        flat.alpha_composite(layers[name])
    # Crop everything to what is visible, plus a small margin.
    box = flat.getbbox()
    m = 24
    box = (max(0, box[0] - m), max(0, box[1] - m), min(size[0], box[2] + m), min(size[1], box[3] + m))
    flat = flat.crop(box)
    layers = {k: v.crop(box) for k, v in layers.items()}
    # The wordmark's centre inside logo.png (the launcher centres the burst there).
    centre = ((ww / 2 - left - box[0]) / flat.width, (wh / 2 - top - box[1]) / flat.height)
    return flat, layers, centre


def build_burst(size=2048, seed=3):
    """Speed lines from an empty middle out to a soft circular edge; turquoise, the line strength in alpha."""
    ss = 2  # drawn at 2x, then reduced, for clean edges
    n = size * ss
    c = n / 2
    img = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(img)
    rng = np.random.default_rng(seed)
    for _ in range(120):
        ang = rng.uniform(0, 2 * np.pi)
        r0 = rng.uniform(0.17, 0.28) * c
        r1 = rng.uniform(0.47, 1.0) * c
        wid = rng.uniform(0.004, 0.02)
        pts = [(c + np.cos(ang) * r0, c + np.sin(ang) * r0),
               (c + np.cos(ang - wid) * r1, c + np.sin(ang - wid) * r1),
               (c + np.cos(ang + wid) * r1, c + np.sin(ang + wid) * r1)]
        d.polygon(pts, fill=int(rng.uniform(90, 255)))
    img = img.resize((size, size), Image.LANCZOS)
    yy, xx = np.mgrid[0:size, 0:size].astype(F)
    r = np.hypot(xx - size / 2, yy - size / 2) / (size / 2)
    fade = np.clip(1.15 - 1.3 * r, 0, 1) ** 1.5
    a = np.asarray(img, F) / 255 * fade
    out = np.zeros((size, size, 4), np.uint8)
    out[..., :3] = TURQUOISE
    out[..., 3] = (a * 255 + 0.5).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


LAYER_NAMES = ("wordmark", "kana shadow", "kana paint", "splatter")


def load_psd(path):
    """The logo's layers from a PSD this script wrote (the four layers by name, any extra ones ignored)."""
    from psd_tools import PSDImage
    psd = PSDImage.open(path)
    size = psd.size
    layers = {}
    for layer in psd.descendants():
        if layer.name in LAYER_NAMES and layer.name not in layers:
            canvas = Image.new("RGBA", size, (0, 0, 0, 0))
            im = layer.topil()
            if im is not None:
                canvas.alpha_composite(im.convert("RGBA"), (max(0, layer.left), max(0, layer.top)))
            layers[layer.name] = canvas
    missing = [n for n in LAYER_NAMES if n not in layers]
    if missing:
        raise SystemExit(f"{path}: layers missing: {', '.join(missing)}")
    return layers


def split_layers(layers):
    """logo.png (the wordmark on the whole canvas), kana.png (shadow + paint + splatter, cropped) and the
    kana's rectangle as fractions of logo.png (left, top, right, bottom; top is negative when they stick out)."""
    size = layers["wordmark"].size
    kana = Image.new("RGBA", size, (0, 0, 0, 0))
    for name in ("kana shadow", "kana paint", "splatter"):
        kana.alpha_composite(layers[name])
    box = kana.getbbox()
    rect = (box[0] / size[0], box[1] / size[1], box[2] / size[0], box[3] / size[1])
    return layers["wordmark"], kana.crop(box), rect


def save_psd(layers, path):
    from psd_tools import PSDImage
    from psd_tools.api.layers import PixelLayer
    first = next(iter(layers.values()))
    psd = PSDImage.new("RGBA", first.size)
    for name, im in layers.items():
        psd.append(PixelLayer.frompil(im, psd, name))
    psd.save(path)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wordmark", help="the wordmark render (transparent PNG)")
    ap.add_argument("--kana", help="the brush kana (only its alpha is used)")
    ap.add_argument("--layers", help="rebuild logo.png and kana.png from this layered PSD instead")
    ap.add_argument("--out", default=os.path.join(here, "..", "..", "resources", "images"))
    ap.add_argument("--psd", help="also write the logo's layers as a PSD here")
    ap.add_argument("--seed", type=int, default=1, help="splatter/paint randomness")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    if args.layers:
        layers = load_psd(args.layers)
        centre = None
    elif args.wordmark and args.kana:
        _, layers, centre = build_logo(args.wordmark, args.kana, args.seed)
    else:
        ap.error("pass --wordmark and --kana, or --layers")
    word, kana, rect = split_layers(layers)
    bleed(word).save(os.path.join(args.out, "logo.png"), optimize=True)
    bleed(kana).save(os.path.join(args.out, "kana.png"), optimize=True)
    build_burst().save(os.path.join(args.out, "burst.png"), optimize=True)
    if args.psd:
        save_psd(layers, args.psd)
    print(f"logo.png {word.width}x{word.height}; kana.png {kana.width}x{kana.height}")
    if centre is not None:
        print(f"wordmark centre at {centre[0]:.3f}, {centre[1]:.3f} of logo.png (kLogoCentreX/Y)")
    print("kana rectangle (kKanaRect): " + ", ".join(f"{v:.4f}" for v in rect))


if __name__ == "__main__":
    main()
