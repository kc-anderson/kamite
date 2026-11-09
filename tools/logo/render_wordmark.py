"""Renders the GOD HAND wordmark (black faces, dark orange bevelled rim, carved cracks) from K's letter SVG and a
layered crack PSD that K paints by hand.

  python tools/logo/render_wordmark.py --svg GOD_HAND.svg --layers wordmark_cracks.psd --out wordmark.png
  python tools/logo/render_wordmark.py --svg GOD_HAND.svg --guide guide.png      (just the letter guide)

The PSD's layers, read by name (alpha is what counts, the colour does not):
  cracks   the crack lines: paint to add, erase to remove (about 8-14 px wide at the default width)
  chips    patches where the black skin has flaked off and the orange shows (soft brush = shallow chip)
  notches  bites taken out of the letters' edges
anything else (the black background, "letters (guide)", "render (reference)") is ignored.

The letters are spread apart by twice the rim's outer width so the rims keep the SVG's gaps; the rim sits half
outside the SVG outline and half inside (fully outside would close the G's mouth and the H / A / N notches), and points
inward round the counters. Output: the wordmark on a transparent canvas, plus <out>_prev.jpg on grey.
Needs numpy, scipy, pillow, psd-tools and rsvg-convert.
"""
import argparse
import subprocess
import tempfile

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

F = np.float32
SVG_WIDTH_UNITS = 535.0
PAD_UNITS = 10.0
RIM_OUT = 2.0                      # rim outside the SVG outline (units)
BEVEL = (2.3, 3.0, 4.3)            # outer slope ends, plateau ends, inner slope ends (units from the rim's edge)
LIGHT = (-0.55, -0.70, 0.42)
LAYERS = ("cracks", "chips", "notches")

# Lit copper, dark to bright (taken from the KAMITE wordmark's pixels).
RAMP = """
341003 381003 371103 391203 3d1203 3e1304 401404 411504 441504 461604 491604 4b1704 4e1805 501905 531a05 561b06
591d05 5e1e06 612006 662107 6a2406 702607 762907 7c2c07 832f08 893208 8f3409 953709 9b3a09 a03c0a a53f0a a9420a
ae440b b2470c b54a0c b84d0d bc500e bf530f c15610 c45910 c65c12 c85f12 cb6113 cd6414 cf6715 d26a16 d46d17 d67018
d8731a da761b dc791d de7b1f e17f20 e38221 e58524 e78928 e88d2c ea922f ec9633 ee9b39 f1a13f f4a949 fbb857 ffe0a5
"""
LUT = np.array([[int(h[i:i + 2], 16) for i in (0, 2, 4)] for h in RAMP.split()], F)


def ramp(x):
    x = np.clip(x, 0, 1) * (len(LUT) - 1)
    i = np.floor(x).astype(int)
    j = np.minimum(i + 1, len(LUT) - 1)
    f = (x - i)[..., None]
    return LUT[i] * (1 - f) + LUT[j] * f


def sm(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)


def fbm(shape, cell, s, rng):
    t = np.zeros(shape, F)
    a = 1.0
    while cell * s >= 2:
        g = rng.standard_normal((int(shape[0] / (cell * s)) + 2, int(shape[1] / (cell * s)) + 2)).astype(F)
        t += a * ndi.zoom(g, (shape[0] / (g.shape[0] - 1), shape[1] / (g.shape[1] - 1)), order=3)[:shape[0], :shape[1]]
        cell /= 2
        a *= 0.5
    return t / t.std()


def layout(svg, width):
    """The letters rasterised at `width` px for the SVG's width, padded, spread apart; returns (letters, grown, s)."""
    with tempfile.NamedTemporaryFile(suffix=".png") as tmp:
        subprocess.run(["rsvg-convert", "-w", str(width), svg, "-o", tmp.name], check=True)
        m = np.array(Image.open(tmp.name).convert("RGBA"))[..., 3] > 127
    s = width / SVG_WIDTH_UNITS
    pad = int(PAD_UNITS * s)
    m = np.pad(m, pad)
    rp = int(round(RIM_OUT * s))
    lab, _ = ndi.label(ndi.binary_fill_holes(m))
    spans = sorted((o[1].start, o[1].stop) for o in ndi.find_objects(lab))
    cuts = [0] + [(spans[i][1] + spans[i + 1][0]) // 2 for i in range(len(spans) - 1)] + [m.shape[1]]
    out = np.zeros((m.shape[0], m.shape[1] + 2 * rp * (len(spans) - 1)), bool)
    for i in range(len(spans)):
        out[:, cuts[i] + 2 * rp * i:cuts[i + 1] + 2 * rp * i] = m[:, cuts[i]:cuts[i + 1]]
    outside = ~ndi.binary_fill_holes(out)
    grown = out | ((ndi.distance_transform_edt(~out) < rp) & outside)
    return out, grown, s


def guide(letters, grown):
    """Cyan lines on the letters' SVG outline and the rim's outer edge, on transparent."""
    g = np.zeros(letters.shape + (4,), np.uint8)
    for mask, a in ((letters, 255), (grown, 140)):
        edge = mask & ~ndi.binary_erosion(mask, iterations=2)
        g[edge] = (0, 220, 255, a)
    return Image.fromarray(g, "RGBA")


def read_layers(path, size):
    from psd_tools import PSDImage
    psd = PSDImage.open(path)
    if psd.size != size:
        raise SystemExit(f"{path}: canvas is {psd.size}, the layout at this width is {size}")
    found = {}
    for layer in psd.descendants():
        if layer.name in LAYERS and layer.name not in found:
            canvas = Image.new("RGBA", size, (0, 0, 0, 0))
            im = layer.topil()
            if im is not None:
                canvas.alpha_composite(im.convert("RGBA"), (max(0, layer.left), max(0, layer.top)))
            found[layer.name] = np.array(canvas)[..., 3].astype(F) / 255
    missing = [n for n in LAYERS if n not in found]
    if missing:
        raise SystemExit(f"{path}: layers missing: {', '.join(missing)}")
    return found


def render(grown, s, cracks, chips, notches, seed=1):
    rng = np.random.default_rng(seed)
    H, W = grown.shape
    inside = ndi.binary_opening(grown & ~(notches > 0.5), iterations=1)
    c = cracks > 0.5
    dc = ndi.distance_transform_edt(~c).astype(F) / s
    d = ndi.distance_transform_edt(inside).astype(F) / s
    b1, b2, b3 = BEVEL
    h = np.where(d < b1, sm(d / b1), 1.0)
    h = np.where(d > b2, 1.0 - 0.7 * sm((d - b2) / (b3 - b2)), h)
    face = d > b3 - 0.2
    chip = chips * face
    crack = sm(1 - dc / 1.4) * inside
    rough = fbm((H, W), 1.2, s, rng)
    grain = fbm((H, W), 0.8, s, rng)
    h = h - 0.35 * chip - 0.75 * crack + 0.03 * rough * (chip > 0.3)
    h3 = ndi.gaussian_filter(h, 0.25 * s) * s * 6.0
    hy, hx = np.gradient(h3)
    n = np.dstack([-hx, -hy, np.ones_like(h3)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    light = np.array(LIGHT, F) / np.linalg.norm(LIGHT)
    half = light + np.array([0, 0, 1], F)
    half /= np.linalg.norm(half)
    diff = np.clip((n * light).sum(2), 0, 1)
    spec = np.clip((n * half).sum(2), 0, 1) ** 12
    copper = np.clip(np.maximum(sm((1 - n[..., 2]) * 6), np.where(face, np.maximum(chip * 1.3, crack), 1.0)), 0, 1)
    cu = ramp(0.03 + 0.9 * diff ** 1.6 + 0.9 * spec)
    black = np.array([12, 8, 7], F) + (grain * 3)[..., None] + spec[..., None] * np.array([70, 40, 25], F)
    col = black * (1 - copper[..., None]) + cu * copper[..., None]
    col *= (1 - 0.55 * sm(1 - dc / 0.35))[..., None]
    edge = (~inside) & (ndi.distance_transform_edt(~inside) / s < 0.7)
    col[edge] = (25, 10, 4)
    alpha = (inside | edge).astype(F) * 255
    return Image.fromarray(np.dstack([np.clip(col, 0, 255), alpha]).astype(np.uint8), "RGBA")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--svg", required=True, help="K's letter SVG (GOD HAND)")
    ap.add_argument("--layers", help="the crack PSD (cracks / chips / notches layers)")
    ap.add_argument("--out", help="the wordmark PNG to write")
    ap.add_argument("--guide", help="write only the letter guide PNG here")
    ap.add_argument("--width", type=int, default=3500, help="px for the SVG's width (the PSD must match)")
    a = ap.parse_args()
    letters, grown, s = layout(a.svg, a.width)
    if a.guide:
        guide(letters, grown).save(a.guide)
    if a.layers and a.out:
        L = read_layers(a.layers, (grown.shape[1], grown.shape[0]))
        img = render(grown, s, L["cracks"], L["chips"], L["notches"])
        img.save(a.out)
        prev = Image.new("RGBA", img.size, (40, 40, 40, 255))
        prev.alpha_composite(img)
        prev.convert("RGB").save(a.out.rsplit(".", 1)[0] + "_prev.jpg", quality=90)


if __name__ == "__main__":
    main()
