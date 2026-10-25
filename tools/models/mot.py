"""MOT motions (bone animation curves): read, sample per frame or between frames, list.

Ported from anasrar/chihuahua pkg/mot (MIT, Copyright (c) 2024 Anas Rin; docs/licenses/chihuahua-MIT.txt), with the
16-bit float of PlatinumGames' engines (sign 1, exponent 6 with bias 47, mantissa 9; also in Blen2MOT and
NieR2Blender2NieR). Standard library only.

Layout: "mtb3", u16 frame count, u8 record count, u8 loop flag, then 12-byte records (u8 bone, u8 channel, u16 curve
count, u32 flags ("use global transform"), u32 payload). Channel: high nibble = encoding (0x00 constant: payload is
an f32; 0x10 Hermite: payload is the offset of the curve block), low nibble = component (0..2 translation x/y/z,
3..5 rotation x/y/z, radians); 0xFF ends the table. Curve block: six half floats (position, position delta,
tangent0, tangent0 delta, tangent1, tangent1 delta), then 4-byte keys (u8 frames since the last key, u8 value step,
u8 incoming tangent step, u8 outgoing tangent step): value = position + delta * step, and the segment between two keys
is a cubic Hermite with the first key's outgoing and the second key's incoming tangent.

  python -I tools/models/mot.py <file> [offset]      list the records and the first frames of each curve
"""
import math, struct, sys

SIGNATURE = b'mtb3'
SENTINEL = 0xFF
COMPONENTS = ('tx', 'ty', 'tz', 'rx', 'ry', 'rz')


def half(v):
    """PlatinumGames' 16-bit float."""
    sign = (v >> 15) & 1
    expo = (v >> 9) & 0x3F
    mant = v & 0x1FF
    if expo == 0 and mant == 0:
        return 0.0
    if expo == 63:
        return float('nan') if mant else (-math.inf if sign else math.inf)
    bits = (sign << 31) | ((expo - 47 + 127) << 23) | (mant << 14)
    return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]


def hermite(p0, m0, p1, m1, t):
    t2 = t * t
    t3 = t2 * t
    return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1


class Record:
    def __init__(self, bone, channel, curve_count, flags, payload):
        self.bone = bone            # the bone index as stored (chihuahua shows it +1)
        self.channel = channel
        self.curve_count = curve_count
        self.flags = flags
        self.payload = payload
        self.sentinel = channel == SENTINEL
        self.constant = None
        self.keys = []              # (frame, value, tangent_in, tangent_out), frame absolute

    @property
    def encoding(self):
        return self.channel & 0xF0

    @property
    def component(self):
        return self.channel & 0x0F

    def sample(self, t):
        """The value at motion time t (frames; fractions sample between frames)."""
        if self.constant is not None:
            return self.constant
        if not self.keys:
            return 0.0
        if t < self.keys[0][0]:
            return self.keys[0][1]
        # The segment starts at the last key at or before t (of several keys on one frame, the last one counts).
        i = 0
        while i + 1 < len(self.keys) and self.keys[i + 1][0] <= t:
            i += 1
        if i + 1 == len(self.keys):
            return self.keys[-1][1]
        (f0, v0, _, m0), (f1, v1, m1, _) = self.keys[i], self.keys[i + 1]
        return hermite(v0, m0, v1, m1, (t - f0) / (f1 - f0))

    def frames(self, count):
        """One value per whole frame, as chihuahua's Quantize."""
        return [self.sample(float(f)) for f in range(count)]


class Mot:
    def __init__(self, data, offset=0, size=None):
        b = data
        o = offset
        if b[o:o + 4] != SIGNATURE:
            raise ValueError('not a MOT (signature %r)' % b[o:o + 4])
        size = len(b) - o if size is None else size
        self.frame_count, self.record_count, self.loop = struct.unpack_from('<HBB', b, o + 4)
        self.records = []
        for i in range(self.record_count):
            bone, channel, count, flags, payload = struct.unpack_from('<BBHII', b, o + 8 + 12 * i)
            r = Record(bone, channel, count, flags, payload)
            if r.sentinel:
                pass
            elif r.encoding == 0x00:
                r.constant = struct.unpack('<f', struct.pack('<I', payload))[0]
            elif r.encoding == 0x10:
                end = payload + 12 + 4 * count
                if payload < 8 + 12 * self.record_count or end > size:
                    raise ValueError('record %d: curve block 0x%x..0x%x outside the motion' % (i, payload, end))
                pos, dpos, t0, dt0, t1, dt1 = (half(x) for x in struct.unpack_from('<6H', b, o + payload))
                frame = 0
                for k in range(count):
                    df, cp, ct0, ct1 = b[o + payload + 12 + 4 * k:o + payload + 16 + 4 * k]
                    frame += df
                    r.keys.append((frame, pos + dpos * cp, t0 + dt0 * ct0, t1 + dt1 * ct1))
            else:
                raise ValueError('record %d: unknown encoding 0x%02x' % (i, r.encoding))
            self.records.append(r)

    def pose(self, t, bones):
        """[(translation, rotation)] per bone at time t; components without a record are 0."""
        out = [([0.0] * 3, [0.0] * 3) for _ in range(bones)]
        for r in self.records:
            if r.sentinel or r.component > 5 or r.bone >= bones:
                continue
            v = r.sample(t)
            (out[r.bone][0] if r.component < 3 else out[r.bone][1])[r.component % 3] = v
        return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    data = open(argv[1], 'rb').read()
    m = Mot(data, int(argv[2], 0) if len(argv) > 2 else 0)
    print('%d frames, %d records, loop %d' % (m.frame_count, m.record_count, m.loop))
    for r in m.records:
        if r.sentinel:
            print('  end')
            continue
        name = COMPONENTS[r.component] if r.component < 6 else 'c%d' % r.component
        if r.constant is not None:
            print('  bone %2d %s constant %.4f' % (r.bone, name, r.constant))
        else:
            print('  bone %2d %s %d keys, frames 0..%d: %s' % (r.bone, name, len(r.keys), r.keys[-1][0] if r.keys else 0,
                  ' '.join('%.3f' % v for v in r.frames(min(m.frame_count, 6)))))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
