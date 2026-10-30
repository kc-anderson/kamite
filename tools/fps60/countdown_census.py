#!/usr/bin/env python3
"""List every countdown site (claude/fps60-notes.md family 1) in a PS2 ELF: an `addiu rt, rs, -1` with a load of rs
and a store of rt to the same base+offset within eight instructions. At 60 FPS each such counter, if it runs once a
game frame, runs twice as fast; the sites of one function/class are fixed together with the km_fps60_countdown hooks.

Usage: countdown_census.py <SLUS_215.03> [recomp/kamite_functions.csv] [--function NAME] [--range LO HI]
Prints the per-function counts, or with --function/--range every site (address, field, load kind, whether the
addiu sits in a branch delay slot, and the register pair).
"""
import bisect, csv, struct, sys
from collections import Counter

LOADS = {0x20: 'lb', 0x24: 'lbu', 0x21: 'lh', 0x25: 'lhu', 0x23: 'lw'}
STORES = {0x28: 'sb', 0x29: 'sh', 0x2B: 'sw'}
BRANCH_OPS = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17, 0x11}

def load_elf(path):
    d = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', d, 0x1C)[0]
    phnum = struct.unpack_from('<H', d, 0x2C)[0]
    segs = []
    for i in range(phnum):
        t, off, va, pa, fs, ms, fl, al = struct.unpack_from('<8I', d, phoff + 32 * i)
        if t == 1:
            segs.append((va, off, fs))
    def rd(a):
        for va, off, fs in segs:
            if va <= a < va + fs:
                return struct.unpack_from('<I', d, off + a - va)[0]
        return 0
    return segs, rd

def is_branch(w):
    op = w >> 26
    if op == 0 and (w & 0x3F) in (0x08, 0x09):  # jr, jalr
        return True
    if op == 0x11:  # COP1: only bc1f / bc1t (rs = 8) branch; mtc1, add.s & co do not
        return ((w >> 21) & 31) == 8
    return op in BRANCH_OPS

def main():
    args = sys.argv[1:]
    segs, rd = load_elf(args[0])
    names = {}
    if len(args) > 1 and args[1].endswith('.csv'):
        for r in csv.reader(open(args[1])):
            try:
                names[int(r[1], 16)] = r[0]
            except (ValueError, IndexError):
                pass
    keys = sorted(names)
    def fn(a):
        i = bisect.bisect_right(keys, a) - 1
        return names[keys[i]] if i >= 0 else '?'
    want = args[args.index('--function') + 1] if '--function' in args else None
    lo, hi = (int(args[args.index('--range') + 1], 16), int(args[args.index('--range') + 2], 16)) if '--range' in args else (0, 1 << 32)
    sites = []
    for va, off, fs in segs:
        for a in range(va + 16, va + fs - 16, 4):
            w = rd(a)
            if w >> 26 != 9 or (w & 0xFFFF) != 0xFFFF:
                continue
            rs, rt = (w >> 21) & 31, (w >> 16) & 31
            ld = None
            # Up to 8 instructions back (0x10D2CC's lw sat 5 before its addiu; the old 3-instruction window missed it),
            # stopping at any other instruction that writes rs.
            for b in range(a - 4, a - 36, -4):
                x = rd(b)
                if x >> 26 in LOADS and ((x >> 16) & 31) == rs:
                    ld = (b, (x >> 21) & 31, x & 0xFFFF, LOADS[x >> 26])
                    break
                op = x >> 26
                dest = (x >> 11) & 31 if op == 0 else (x >> 16) & 31 if 8 <= op <= 15 or op in LOADS else -1
                if dest == rs:
                    break
            if not ld:
                continue
            for c in range(a + 4, a + 36, 4):
                x = rd(c)
                if x >> 26 in STORES and ((x >> 16) & 31) == rt and ((x >> 21) & 31) == ld[1] and (x & 0xFFFF) == ld[2]:
                    sites.append((a, fn(a), ld[3], ld[2], ld[1], rs, rt, is_branch(rd(a - 4))))
                    break
    if want or '--range' in args:
        for a, f, kind, field, base, rs, rt, delay in sites:
            if (want and f != want) or not (lo <= a < hi):
                continue
            print(f'0x{a:08X} {f:40s} {kind:3s} +0x{field:X}(r{base}) addiu r{rt},r{rs},-1{"  DELAY SLOT" if delay else ""}')
        return
    print(len(sites), 'countdown sites')
    for f, n in Counter(s[1] for s in sites).most_common(60):
        print(f'{n:4d} {f}')

if __name__ == '__main__':
    main()
