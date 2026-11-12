# Grades the OPTIONS wall to the game's own Options wall so the labels read as well as on the PS2 (K, Nov 12: ours
# was brighter and more saturated). In CIELAB: lightness mapped by quantiles onto the game wall's (brightness and
# contrast), chroma scaled to the game wall's mean (saturation), hue kept. The game's statistics come from the backdrop
# texture in your own disc's id/local/us/idsubscroption_us.idd (its painted text and title left out); K's wall is
# measured on its 16:9 middle at the game's 448-line scale. Nothing of the game's art goes into the output.
#   python3 grade_wall.py wall.jpg idsubscroption_us.idd wall_graded.jpg      (after compose_wall.py)
import struct, sys
import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion
from skimage import color

B32 = [[0,1,4,5,16,17,20,21],[2,3,6,7,18,19,22,23],[8,9,12,13,24,25,28,29],[10,11,14,15,26,27,30,31]]
C32 = [[0,1,4,5,8,9,12,13],[2,3,6,7,10,11,14,15],[16,17,20,21,24,25,28,29],[18,19,22,23,26,27,30,31],
       [32,33,36,37,40,41,44,45],[34,35,38,39,42,43,46,47],[48,49,52,53,56,57,60,61],[50,51,54,55,58,59,62,63]]
C8 = [[0,4,16,20,32,36,48,52,2,6,18,22,34,38,50,54],[8,12,24,28,40,44,56,60,10,14,26,30,42,46,58,62],
      [33,37,49,53,1,5,17,21,35,39,51,55,3,7,19,23],[41,45,57,61,9,13,25,29,43,47,59,63,11,15,27,31],
      [96,100,112,116,64,68,80,84,98,102,114,118,66,70,82,86],[104,108,120,124,72,76,88,92,106,110,122,126,74,78,90,94],
      [65,69,81,85,97,101,113,117,67,71,83,87,99,103,115,119],[73,77,89,93,105,109,121,125,75,79,91,95,107,111,123,127],
      [128,132,144,148,160,164,176,180,130,134,146,150,162,166,178,182],[136,140,152,156,168,172,184,188,138,142,154,158,170,174,186,190],
      [161,165,177,181,129,133,145,149,163,167,179,183,131,135,147,151],[169,173,185,189,137,141,153,157,171,175,187,191,139,143,155,159],
      [224,228,240,244,192,196,208,212,226,230,242,246,194,198,210,214],[232,236,248,252,200,204,216,220,234,238,250,254,202,206,218,222],
      [193,197,209,213,225,229,241,245,195,199,211,215,227,231,243,247],[201,205,217,221,233,237,249,253,203,207,219,223,235,239,251,255]]

def game_wall(idd):
    d = open(idd, 'rb').read()
    tex = struct.unpack_from('<I', d, 8)[0]
    hdr = tex + struct.unpack_from('<I', d, tex + 4)[0]
    payload = d[hdr + 0xE0:hdr + 0xE0 + 0x40000]
    clut = d[hdr + 0xE0 + 0x40100:hdr + 0xE0 + 0x40100 + 1024]
    mem = bytearray(0x40000)
    for y in range(1024):
        for x in range(64):
            o = ((y // 32) * 32 + B32[(y % 32) // 8][x // 8]) * 256 + C32[y % 8][x % 8] * 4
            mem[o:o + 4] = payload[(y * 64 + x) * 4:(y * 64 + x) * 4 + 4]
    rgb = np.zeros((448, 512, 3), np.uint8)
    for y in range(448):
        for x in range(512):
            k = mem[((y // 64) * 4 + x // 128) * 32 * 256 + B32[(y % 64) // 16][(x % 128) // 16] * 256 + C8[y % 16][x % 16]]
            s = (k & 0xE7) | ((k & 8) << 1) | ((k & 0x10) >> 1)
            rgb[y, x] = tuple(clut[s * 4:s * 4 + 3])
    a = rgb.astype(int)
    tan = (abs(a[..., 0] - 188) < 40) & (abs(a[..., 1] - 121) < 40) & (abs(a[..., 2] - 72) < 40) & (a[..., 0] > 150)
    grey = (abs(a[..., 0] - 135) < 30) & (abs(a[..., 1] - 119) < 30) & (abs(a[..., 2] - 109) < 30) & (a[..., 0] > 110)
    keep = ~(tan | grey)
    keep[:80] = False  # the painted OPTIONS title
    return rgb, binary_erosion(keep, iterations=3)

wall, idd, out = sys.argv[1:4]
rgb, keep = game_wall(idd)
game = color.rgb2lab(rgb / 255.0)[keep]
img = Image.open(wall).convert('RGB')
W, H = img.size
cw = round(H * 16 / 9)
ours = color.rgb2lab(np.asarray(img.crop(((W - cw) // 2, 0, (W - cw) // 2 + cw, H)).resize((682, 448), Image.LANCZOS)) / 255.0).reshape(-1, 3)
qs = np.linspace(0, 1, 257)
q_ours, q_game = np.quantile(ours[:, 0], qs), np.quantile(game[:, 0], qs)
chroma = np.hypot(game[:, 1], game[:, 2]).mean() / np.hypot(ours[:, 1], ours[:, 2]).mean()
full = np.asarray(img) / 255.0
res = np.empty_like(full)
for y0 in range(0, H, 256):
    lab = color.rgb2lab(full[y0:y0 + 256])
    lab[..., 0] = np.interp(lab[..., 0], q_ours, q_game)
    lab[..., 1:] *= chroma
    res[y0:y0 + 256] = np.clip(color.lab2rgb(lab), 0, 1)
Image.fromarray((res * 255 + 0.5).astype(np.uint8)).save(out, quality=92, subsampling=0)
print('chroma x%.2f, median lightness %.1f -> %.1f' % (chroma, np.median(ours[:, 0]), np.median(game[:, 0])))
