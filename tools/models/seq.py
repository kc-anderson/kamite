"""SEQ motion events (one per motion: speed windows, sound cues, attack windows, effects): read, list, edit in place.

Format facts from akitotheanimator/God-Hand-Tools Blen2SEQ (no licence: this is our own code from the documented
layout) and the decomp's readers (LucasPicoli/god-hand-decomp: Motion_seq_readSpdData 0x2C7D18, readSeData 0x2C7DA8,
readQuadData 0x2C7E30 (the attack windows), readEffectData 0x2C7F00, used by Motion_seq_ctr 0x2C7FD0; the 60 FPS
mode's hitbox fix at 0x2C81C0 sits in Motion_seq_ctr). Standard library only.

Layout: "SEQ\\0" as a u64, four (u16 offset, u16 count) pairs (speed, sound, attack, effect), a u64 filler, then:
  speed  8 bytes: u16 first frame, u16 last frame, f32 multiplier of the motion's speed between them
  sound 12 bytes: u16 frame, u16 sound id, 8 bytes (zero in the files Blen2SEQ writes)
  attack 8 bytes: u16 first frame, u16 last frame, 4 bytes of parameters (meaning not documented)
  effect 12 bytes: i32 stop (-1 = keeps running, 0 = stops with the motion), u32 frame, u8 effect, s8 bone (-1 =
                   the root), u16 particle
Frames are the motion's own (30 per second at the PS2's rate).

  python -I tools/models/seq.py <file> [offset]                 list
  python -I tools/models/seq.py <file> --speed <i> <multiplier> -o <out>   change speed window i (same size)
"""
import struct, sys

MAGIC = 0x514553  # "SEQ\0\0\0\0\0" as a little-endian u64
KINDS = ('speed', 'sound', 'attack', 'effect')
SIZES = {'speed': 8, 'sound': 12, 'attack': 8, 'effect': 12}


def read(data, offset=0):
    """{'speed': [...], 'sound': [...], 'attack': [...], 'effect': [...]}, each entry a dict with 'at' (its offset)."""
    o = offset
    if struct.unpack_from('<Q', data, o)[0] != MAGIC:
        raise ValueError('not a SEQ')
    out = {}
    for k, kind in enumerate(KINDS):
        table, count = struct.unpack_from('<HH', data, o + 8 + 4 * k)
        rows = []
        for i in range(count):
            at = o + table + SIZES[kind] * i
            if at + SIZES[kind] > len(data):
                raise ValueError('%s %d past the end of the data' % (kind, i))
            if kind == 'speed':
                first, last, mult = struct.unpack_from('<HHf', data, at)
                rows.append(dict(at=at, first=first, last=last, multiplier=mult))
            elif kind == 'sound':
                frame, sound = struct.unpack_from('<HH', data, at)
                rows.append(dict(at=at, frame=frame, sound=sound, extra=bytes(data[at + 4:at + 12])))
            elif kind == 'attack':
                first, last = struct.unpack_from('<HH', data, at)
                rows.append(dict(at=at, first=first, last=last, params=tuple(data[at + 4:at + 8])))
            else:
                stop, frame, effect, bone, particle = struct.unpack_from('<iIBbH', data, at)
                rows.append(dict(at=at, stop=stop, frame=frame, effect=effect, bone=bone, particle=particle))
        out[kind] = rows
    return out


def set_speed(data, seq, index, multiplier):
    """A copy of data with speed window `index` at `multiplier` (the file keeps its size and layout)."""
    b = bytearray(data)
    struct.pack_into('<f', b, seq['speed'][index]['at'] + 4, multiplier)
    return bytes(b)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    data = open(argv[1], 'rb').read()
    args = argv[2:]
    offset = int(args.pop(0), 0) if args and not args[0].startswith('-') else 0
    seq = read(data, offset)
    if args and args[0] == '--speed':
        index, mult = int(args[1]), float(args[2])
        out = args[args.index('-o') + 1]
        open(out, 'wb').write(set_speed(data, seq, index, mult))
        print('speed window %d -> x%g in %s' % (index, mult, out))
        return 0
    for r in seq['speed']:
        print('speed  frames %3d..%3d x%.3f' % (r['first'], r['last'], r['multiplier']))
    for r in seq['sound']:
        print('sound  frame %3d sound %d' % (r['frame'], r['sound']))
    for r in seq['attack']:
        print('attack frames %3d..%3d params %s' % (r['first'], r['last'], ' '.join('%d' % p for p in r['params'])))
    for r in seq['effect']:
        print('effect frame %3d effect %d bone %d particle %d%s' % (r['frame'], r['effect'], r['bone'], r['particle'],
              ' (keeps running)' if r['stop'] == -1 else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
