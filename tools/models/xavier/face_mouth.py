"""Paste the mouth texture into the face texture's unused bottom strip (prep1 maps the mouth interior's UVs there):

    python -I tools/models/xavier/face_mouth.py <work>/textures   (after prep1, before prep2)

XavierFace.png uses v >= 0.407 (its top 61% rows); MouthTexture1.png goes into the bottom MOUTH_V (0.38) of the
height, squeezed (an interior, seen through a half-open mouth: the squeeze does not show). Idempotent: an already
pasted strip is pasted again the same way. Needs Pillow."""
import os, sys
from PIL import Image

MOUTH_V = 0.38

def main(tex):
    face = Image.open(os.path.join(tex, 'XavierFace.png')).convert('RGBA')
    mouth = Image.open(os.path.join(tex, 'MouthTexture1.png')).convert('RGBA')
    W, H = face.size
    h = int(round(H * MOUTH_V))
    face.paste(mouth.resize((W, h), Image.LANCZOS), (0, H - h))
    face.save(os.path.join(tex, 'XavierFace.png'))
    print('XavierFace.png: the mouth texture in the bottom %d of %d rows' % (h, H))

if __name__ == '__main__':
    main(sys.argv[1])
