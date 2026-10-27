"""Paste the mouth texture into the face texture's unused bottom strip (prep1 maps the mouth interior's UVs there):

    python -I tools/models/xavier/face_mouth.py <work>/textures   (after prep1, before prep2)

XavierFace.png uses v >= 0.407 (its top 61% rows); MouthTexture1.png goes into the bottom MOUTH_V (0.38) of the
height, squeezed (an interior, seen through a half-open mouth: the squeeze does not show). Its top row of tooth shapes
(rows 0..STRIP of 512) belonged to the model's separate teeth, which prep2 replaces with two bands: a tooth strip is
painted there for them (gum at the top, then the teeth with gaps between them; prep2 maps both bands' roots to the
gum). A hand-painted strip can replace it: TOOTH_STRIP_PNG (an RGB image, any size, scaled to 512 x STRIP).
Idempotent: reads MouthTexture1.png each time. Needs Pillow."""
import os, sys
from PIL import Image, ImageDraw

MOUTH_V = 0.38
STRIP = 52          # rows of the 512-row mouth texture (prep2 TOOTH_STRIP)
GUM = 10            # the gum's rows at the top of the strip


def tooth_strip(w=512, h=STRIP):
    """Eight teeth, the two front ones widest, under a gum line, at 4x then scaled down (smooth edges)."""
    S = 4
    im = Image.new('RGB', (w * S, h * S), (70, 28, 30))
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, w * S, GUM * S), fill=(176, 84, 90))
    widths = [0.8, 0.95, 1.1, 1.3, 1.3, 1.1, 0.95, 0.8]
    x = 0.0
    total = sum(widths)
    for k, wd in enumerate(widths):
        x0 = x / total * w * S; x1 = (x + wd) / total * w * S; x = x + wd
        gap = 1.2 * S
        top = (GUM - 3) * S
        bottom = h * S - (2 + abs(k - 3.5) * 1.2) * S   # the side teeth a little shorter
        d.rounded_rectangle((x0 + gap, top, x1 - gap, bottom), radius=int((x1 - x0) * 0.28), fill=(236, 230, 214))
        # a soft shade towards the gum
        d.rectangle((x0 + gap, top, x1 - gap, top + 4 * S), fill=(214, 200, 186))
    d.rectangle((0, 0, w * S, (GUM - 3) * S), fill=(176, 84, 90))
    return im.resize((w, h), Image.LANCZOS)


def main(tex):
    face = Image.open(os.path.join(tex, 'XavierFace.png')).convert('RGBA')
    mouth = Image.open(os.path.join(tex, 'MouthTexture1.png')).convert('RGBA').resize((512, 512), Image.LANCZOS)
    custom = os.environ.get('TOOTH_STRIP_PNG')
    strip = Image.open(custom).convert('RGB').resize((512, STRIP), Image.LANCZOS) if custom else tooth_strip()
    mouth.paste(strip, (0, 0))
    W, H = face.size
    h = int(round(H * MOUTH_V))
    face.paste(mouth.resize((W, h), Image.LANCZOS), (0, H - h))
    face.save(os.path.join(tex, 'XavierFace.png'))
    print('XavierFace.png: the mouth texture in the bottom %d of %d rows, the tooth strip on top of it' % (h, H))

if __name__ == '__main__':
    main(sys.argv[1])
