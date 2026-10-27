"""Bake a TrueType font into a BMFont atlas (splash.fnt + splash.png) for the launcher's splash text.

    python -I tools/splash/bake_font.py <font.ttf> <out dir = resources/splash> [--size 128]

The font file itself is not shipped (Dominican's terms: free for any use, the raw .ttf not redistributed); the
launcher loads the baked atlas with raylib's LoadFont. Glyphs: printable ASCII and Latin-1 (U+0020..U+007E,
U+00A0..U+00FF). Each glyph's own ink box is packed with its real height (raylib's own atlas packer assumes glyphs
are no taller than the font size, which decorative fonts break: clipped, garbled splashes). Needs Pillow.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont


def bake(ttf, out_dir, size):
    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()
    codes = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))
    glyphs = []
    notdef = bytes(font.getmask('\u0378'))  # a codepoint no font has: the box drawn for missing glyphs
    for c in codes:
        ch = chr(c)
        if c > 0x7E and bytes(font.getmask(ch)) == notdef:
            continue  # not in the font: raylib draws '?' for it instead of a box
        l, t, r, b = font.getbbox(ch)
        w, h = max(r - l, 0), max(b - t, 0)
        adv = int(round(font.getlength(ch)))
        img = None
        if w and h:
            img = Image.new('L', (w, h), 0)
            ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255)
        glyphs.append((c, l, t, w, h, adv, img))
    # Shelf packing by height, widest atlas 2048; the height then rounded up to a power of two.
    pad = 2
    width = 2048
    order = sorted((g for g in glyphs if g[6] is not None), key=lambda g: -g[4])
    x = y = pad
    shelf = 0
    place = {}
    for g in order:
        c, l, t, w, h, adv, img = g
        if x + w + pad > width:
            x = pad
            y += shelf + pad
            shelf = 0
        place[c] = (x, y)
        x += w + pad
        shelf = max(shelf, h)
    height = 1
    while height < y + shelf + pad:
        height *= 2
    atlas = Image.new('RGBA', (width, height), (255, 255, 255, 0))
    for g in order:
        c, l, t, w, h, adv, img = g
        px, py = place[c]
        white = Image.new('RGBA', img.size, (255, 255, 255, 255))
        white.putalpha(img)
        atlas.paste(white, (px, py))
    os.makedirs(out_dir, exist_ok=True)
    atlas.save(os.path.join(out_dir, 'splash.png'), optimize=True)
    lines = ['info face="%s" size=%d bold=0 italic=0 charset="" unicode=1 stretchH=100 smooth=1 aa=1 padding=0,0,0,0 spacing=0,0 outline=0'
             % (os.path.splitext(os.path.basename(ttf))[0], size),
             'common lineHeight=%d base=%d scaleW=%d scaleH=%d pages=1 packed=0 alphaChnl=0 redChnl=4 greenChnl=4 blueChnl=4'
             % (ascent + descent, ascent, width, height),
             'page id=0 file="splash.png"',
             'chars count=%d' % len(glyphs)]
    for c, l, t, w, h, adv, img in glyphs:
        px, py = place.get(c, (0, 0))
        lines.append('char id=%d x=%d y=%d width=%d height=%d xoffset=%d yoffset=%d xadvance=%d page=0 chnl=15'
                     % (c, px, py, w, h, l, t, adv))
    with open(os.path.join(out_dir, 'splash.fnt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d glyphs, %dx%d, line height %d' % (out_dir, len(glyphs), width, height, ascent + descent))


if __name__ == '__main__':
    args = sys.argv[1:]
    size = 128
    if '--size' in args:
        i = args.index('--size')
        size = int(args[i + 1])
        del args[i:i + 2]
    bake(args[0], args[1] if len(args) > 1 else 'resources/splash', size)
