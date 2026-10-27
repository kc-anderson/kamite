"""Re-make a character's 16-colour textures from its full-resolution ones (the textures/ folder), with a palette that
keeps small but distinct colours (k-means with k-means++ seeding instead of median cut), and rename the full-resolution
images to the new textures' PCSX2 names.

    python -I tools/models/xavier/requantize.py <character folder, e.g. assets/characters/xavier> [slot ...]

The 16-colour textures are what the game draws when no replacement is found: replacement textures switched off, or a
save state made with the character loaded while another is selected. Xavier's body (pl0001) had fifteen browns and one
olive for the background, the snake sleeve and the buttons (Oct 27: "green nipples" and an olive arm). Each slot
keeps its size, header, TEX0 and CLUT place; only the indices and the 16 colours change. Opaque slots only (the
hair's alpha stays as made). Needs numpy, scipy, Pillow and xxhash.
"""
import os, struct, sys
import numpy as np
from PIL import Image
from scipy.cluster.vq import kmeans2

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
from gamemodel import tm3_read, tm3_write
from gametex import tim3_decode, swizzle4
from pcsx2tex import replacement_name


def layout(t):
    p = 16
    total, clut, img = struct.unpack_from('<3I', t, p)
    hs = struct.unpack_from('<H', t, p + 12)[0]
    w, h = struct.unpack_from('<2H', t, p + 20)
    return p + hs, img, p + hs + img, clut, w, h


def quantize(rgb, seed=1, runs=6):
    """16 colours for rgb (N x 3, 0..255), the best of a few k-means++ runs by mean squared error."""
    data = rgb.astype(np.float64)
    best = None
    for r in range(runs):
        np.random.seed(seed + r)
        cent, lab = kmeans2(data, 16, minit='++', iter=30)
        err = ((data - cent[lab]) ** 2).sum(axis=1)
        score = err.mean()
        if best is None or score < best[0]:
            best = (score, cent, lab)
    return np.clip(np.rint(best[1]), 0, 255).astype(np.uint8), best[2].astype(np.uint8)


def requantize(t, hd_path):
    off, img, coff, clut, w, h = layout(t)
    old, info = tim3_decode(t)
    if (old[..., 3] < 254).any():
        return None, 'has alpha: left as it is'
    hd = Image.open(hd_path).convert('RGB').resize((w, h), Image.LANCZOS)
    rgb = np.asarray(hd).reshape(-1, 3)
    pal, idx = quantize(rgb)
    idx = idx.reshape(h, w)
    if w >= 128 and h >= 128:
        data = swizzle4(idx, w, h)
    else:
        f = idx.ravel(); data = bytes((f[0::2] | (f[1::2] << 4)).astype(np.uint8))
    assert len(data) == img and clut == 64, (len(data), img, clut)
    c = bytearray()
    for r, g, b in pal:
        c += bytes((int(r), int(g), int(b), 0x80))
    out = t[:off] + bytes(data) + t[off + img:coff] + bytes(c) + t[coff + clut:]
    new, _ = tim3_decode(out)
    ref = np.asarray(hd).astype(float)
    e_old = np.abs(old[..., :3].astype(float) - ref).mean()
    e_new = np.abs(new[..., :3].astype(float) - ref).mean()
    return out, 'mean error %.1f -> %.1f' % (e_old, e_new)


def main(folder, slots):
    tm3_path = [os.path.join(folder, f) for f in os.listdir(folder) if f.endswith('.tm3')][0]
    texdir = os.path.join(folder, 'textures')
    texs = tm3_read(open(tm3_path, 'rb').read())
    out = []
    for name, data, _offset in texs:
        if slots and name not in slots:
            out.append((name, data)); continue
        old_name = replacement_name(data)
        hd = os.path.join(texdir, old_name + '.png')
        if not os.path.isfile(hd):
            print(name, 'no full-resolution image', old_name); out.append((name, data)); continue
        new, note = requantize(data, hd)
        if new is None:
            print(name, note); out.append((name, data)); continue
        new_name = replacement_name(new)
        os.replace(hd, os.path.join(texdir, new_name + '.png'))
        print(name, note, old_name, '->', new_name)
        out.append((name, new))
    open(tm3_path, 'wb').write(tm3_write(out))


if __name__ == '__main__':
    main(sys.argv[1], set(sys.argv[2:]))
