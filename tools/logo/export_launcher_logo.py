"""Exports the GOD HAND: Rearmed logo from K's layered GHR_logo.psd into the launcher's images.

  python tools/logo/export_launcher_logo.py --psd GHR_logo.psd [--out resources/images] [--width 2800]
                                            [--readme assets/logo.png]

Every image has the PSD's whole canvas (scaled to --width), so the launcher draws them all into one rectangle:
  kana.png          the flat kana (drawn behind the wordmark, moved a little with the cursor)
  logo.png          GOD HAND and its shadow (the PSD's "GOD HAND shadow" + "GOD HAND"; K's layer style rasterised)
  rearmed.png       the REARMED plate's body, without its letters (the frame, the plaque, the letters' drop shadow)
  rearmed_text.png  the plate's letters, white with a slight top-to-bottom shade (the launcher tints them gold)
  rearmed_glow.png  the letters blurred, white (the hot glow when Play is pressed)
The plate is rebuilt here from its recipe (the PSD has it as one layer) and checked against the PSD's "REARMED plate"
layer. Prints the constants launcher.cpp needs (kLogoCentre, kRearmedRect). --readme also writes a flat composite.
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage as ndi

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf"
PLATE_WIDTH = 1900          # the plate's width in the PSD, drop shadow included
FRAME = (214, 150, 62, 255)
DARK = (40, 24, 10, 255)
FACE = (64, 38, 14, 255)


def spaced_mask(text, font, track):
    d = ImageDraw.Draw(Image.new("L", (1, 1)))
    widths = [d.textlength(c, font=font) for c in text]
    im = Image.new("L", (int(sum(widths) + track * (len(text) - 1)) + 20, int(font.size * 1.4)), 0)
    dr = ImageDraw.Draw(im)
    x = 10
    for c, w in zip(text, widths):
        dr.text((x, 10), c, font=font, fill=255)
        x += w + track
    return im.crop(im.getbbox())


def plate_parts(font_path):
    """The plate at its design size: (body RGBA without letters, letters' coverage L), shadow margin included."""
    m = spaced_mask("REARMED", ImageFont.truetype(font_path, 170), 48)
    w, h = m.width + 260, m.height + 150
    body = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(body)
    d.rounded_rectangle((0, 0, w - 1, h - 1), 40, fill=FRAME)
    d.rounded_rectangle((10, 10, w - 11, h - 11), 32, fill=DARK)
    d.rounded_rectangle((22, 22, w - 23, h - 23), 26, outline=FRAME, width=6)
    d.rounded_rectangle((34, 34, w - 35, h - 35), 20, fill=FACE)
    tx, ty = (w - m.width) // 2, (h - m.height) // 2
    shade = Image.new("RGBA", m.size, (20, 10, 0, 0))
    shade.putalpha(m.point(lambda v: int(v * 0.8)))
    body.alpha_composite(shade, (tx + 5, ty + 6))
    letters = Image.new("L", (w, h), 0)
    letters.paste(m, (tx, ty))
    # the plate's own hard drop shadow (10 px, 60%), as in the PSD
    out = Image.new("RGBA", (w + 20, h + 20), (0, 0, 0, 0))
    drop = Image.new("RGBA", body.size, (0, 0, 0, 0))
    drop.putalpha(body.split()[3].point(lambda v: int(v * 0.6)))
    out.alpha_composite(drop, (10, 10))
    out.alpha_composite(body, (0, 0))
    full_letters = Image.new("L", out.size, 0)
    full_letters.paste(letters, (0, 0))
    return out, full_letters


def layer_image(psd, name):
    layer = next(l for l in psd.descendants() if l.name == name)
    canvas = Image.new("RGBA", psd.size, (0, 0, 0, 0))
    im = layer.topil()
    if im is not None:
        canvas.alpha_composite(im.convert("RGBA"), (max(0, layer.left), max(0, layer.top)))
    return canvas


def main():
    from psd_tools import PSDImage
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--psd", required=True)
    ap.add_argument("--out", default="resources/images")
    ap.add_argument("--width", type=int, default=2800)
    ap.add_argument("--font", default=FONT)
    ap.add_argument("--readme", help="also write a flat composite here (1200 px wide)")
    a = ap.parse_args()
    psd = PSDImage.open(a.psd)
    W, H = psd.size
    for name in ("GOD HAND", "GOD HAND shadow"):
        layer = next(l for l in psd.descendants() if l.name == name)
        if layer.has_effects():
            raise SystemExit(f"'{name}' still has a live layer style: rasterise it in Photoshop and save")
    kana = layer_image(psd, "kana")
    logo = layer_image(psd, "GOD HAND shadow")
    logo.alpha_composite(layer_image(psd, "GOD HAND"))
    plate_layer = layer_image(psd, "REARMED plate")

    # the plate, rebuilt at the PSD's scale and placed on the PSD's plate
    body, letters = plate_parts(a.font)
    scale = PLATE_WIDTH / body.width
    size = (PLATE_WIDTH, int(round(body.height * scale)))
    body = body.resize(size, Image.LANCZOS)
    letters = letters.resize(size, Image.LANCZOS)
    pb = plate_layer.getbbox()
    gb = Image.alpha_composite(body, Image.merge("RGBA", (letters,) * 4)).getbbox()
    ox, oy = pb[0] - gb[0], pb[1] - gb[1]
    rebuilt = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    rebuilt.alpha_composite(body, (ox, oy))
    gold = Image.new("RGBA", size, (240, 206, 90, 0))
    gold.putalpha(letters)
    rebuilt.alpha_composite(gold, (ox, oy))
    diff = np.abs(np.array(rebuilt, np.float32)[..., 3] - np.array(plate_layer, np.float32)[..., 3])
    print(f"plate rebuilt at ({ox}, {oy}); alpha differs from the PSD's layer by {diff.mean():.3f} on average")
    if diff.mean() > 2.0:
        print("WARNING: the PSD's plate no longer matches the recipe (K edited it?): check rearmed*.png")

    body_full = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    body_full.alpha_composite(body, (ox, oy))
    lt = np.zeros((H, W), np.float32)
    lt[oy:oy + size[1], ox:ox + size[0]] = np.array(letters, np.float32) / 255
    ys = np.arange(H, dtype=np.float32)[:, None]
    rows = np.where(lt.max(1) > 0)[0]
    top, bottom = rows.min(), rows.max()
    shadeband = np.clip(1.05 - 0.25 * (ys - top) / max(1, bottom - top), 0.0, 1.0) * np.ones((1, W), np.float32)
    text = np.dstack([np.clip(shadeband * 255, 0, 255)] * 3 + [lt * 255]).astype(np.uint8)
    glow = ndi.gaussian_filter(lt, 18.0 * W / 4872)
    glow = glow / max(glow.max(), 1e-6)
    glow_img = np.dstack([np.full((H, W), 255, np.float32)] * 3 + [glow * 255]).astype(np.uint8)

    out_size = (a.width, int(round(H * a.width / W)))
    os.makedirs(a.out, exist_ok=True)
    for name, im in (("kana.png", kana), ("logo.png", logo), ("rearmed.png", body_full),
                     ("rearmed_text.png", Image.fromarray(text, "RGBA")),
                     ("rearmed_glow.png", Image.fromarray(glow_img, "RGBA"))):
        im.resize(out_size, Image.LANCZOS).save(os.path.join(a.out, name), optimize=True)
        print("wrote", os.path.join(a.out, name), out_size)

    gbox = layer_image(psd, "GOD HAND").getbbox()
    pbody = body_full.getbbox()
    print(f"constexpr float kLogoCentreX = {(gbox[0] + gbox[2]) / 2 / W:.4f}f, "
          f"kLogoCentreY = {(gbox[1] + gbox[3]) / 2 / H:.4f}f;")
    print(f"constexpr float kRearmedRect[4] = {{{pbody[0] / W:.4f}f, {pbody[1] / H:.4f}f, "
          f"{(pbody[2] - 10 * scale) / W:.4f}f, {(pbody[3] - 10 * scale) / H:.4f}f}};")
    if a.readme:
        flat = kana.copy()
        flat.alpha_composite(logo)
        flat.alpha_composite(rebuilt)
        flat = flat.crop(flat.getbbox())
        flat.resize((1200, int(round(flat.height * 1200 / flat.width))), Image.LANCZOS).save(a.readme, optimize=True)
        print("wrote", a.readme)


if __name__ == "__main__":
    main()
