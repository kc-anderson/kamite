"""A character's hand-painted layers over the game's textures (build_characters.py, "overlay" in character.json): e.g.
Xavier's fur where Gene's gauntlet arm comes out of his stump, painted over the arm skin.

overlay(base_rgba, layer_rgba) -> the layer (an RGBA image, any size: scaled to the base) laid over the base by its alpha.
overlay_textures(game_texs, layers) -> {name: new tim3 bytes}: the game's named 16-colour textures with their layers
    on, requantized to 16 colours in the game's size and layout.
"""
import math
import struct

import numpy as np
from PIL import Image


def overlay(base_rgba, layer_rgba):
    h, w = base_rgba.shape[:2]
    layer = Image.fromarray(np.ascontiguousarray(layer_rgba), 'RGBA')
    if layer.size != (w, h):
        # premultiplied, so the transparent texels' colours do not bleed into the edge
        pre = np.asarray(layer).astype(np.float32)
        pre[..., :3] *= pre[..., 3:4] / 255.0
        small = np.stack([np.asarray(Image.fromarray(pre[..., c]).resize((w, h), Image.LANCZOS)) for c in range(4)], -1)
        a = np.clip(small[..., 3:4], 0, 255) / 255.0
        rgb = np.where(a > 1e-4, small[..., :3] / np.maximum(a, 1e-4), 0.0)
    else:
        arr = np.asarray(layer).astype(np.float32)
        a = arr[..., 3:4] / 255.0
        rgb = arr[..., :3]
    out = base_rgba.astype(np.float32)
    out[..., :3] = out[..., :3] * (1.0 - a) + np.clip(rgb, 0, 255) * a
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def quantize16(rgba):
    """16-colour indices and palette (RGBA, alpha 255) of an opaque image."""
    q = Image.fromarray(np.ascontiguousarray(rgba[..., :3]), 'RGB').quantize(16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal3 = np.array(q.getpalette()[:48], np.uint8).reshape(-1, 3)
    while len(pal3) < 16:
        pal3 = np.vstack([pal3, pal3[-1:]])
    pal = np.hstack([pal3[:16], np.full((16, 1), 255, np.uint8)])
    d = ((rgba[:, :, None, :3].astype(int) - pal[None, None, :, :3].astype(int)) ** 2).sum(-1)
    return np.argmin(d, -1).astype(np.uint8), pal


def tim3_encode(idx, pal, w, h):
    """A 4-bit TIM3 in the game's layout (as encode_xavier.py writes them)."""
    from gametex import swizzle4
    idx = np.asarray(idx, np.uint8)
    if w >= 128 and h >= 128:
        data = swizzle4(idx, w, h)
        fid = 6
    else:
        f = idx.ravel()
        data = bytes((f[0::2] | (f[1::2] << 4)).astype(np.uint8))
        fid = 0
    clut = bytearray()
    for r, g, b, a in pal:
        clut += bytes((int(r), int(g), int(b), int(round(int(a) * 128 / 255))))
    tbw = max(2, w // 64)
    tw = int(math.log2(w))
    th = int(math.log2(h))
    tex0 = (tbw << 14) | (20 << 20) | (tw << 26) | (th << 30)
    pic = struct.pack('<3I2H4B2H2Q2I', 48 + len(clut) + len(data), len(clut), len(data), 48, 16, 0, 1, 3, 4, w, h, tex0, 0x260, 0, 0)
    return b'TIM3' + bytes((4, fid, 1, 0)) + bytes(8) + pic + data + bytes(clut)


def overlay_textures(game_texs, layers):
    from gametex import tim3_decode
    out = {}
    for name, t in game_texs:
        if name in layers:
            rgba, info = tim3_decode(t)
            idx, pal = quantize16(overlay(rgba, layers[name]))
            out[name] = tim3_encode(idx, pal, info['w'], info['h'])
    return out
