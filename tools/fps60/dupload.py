#!/usr/bin/env python3
"""Copies of a hooked instruction in branch delay slots that jump past it (claude/fps60-notes.md family 1).

A countdown `lw v0,X(sN); beqz v0; addiu v0,v0,-1` is hooked at its load (load-raise), but other paths reach the
decrement through `bnel/beql/b ... lw v0,X(sN)` whose target is the instruction after the hooked load: on those paths
the countdown is not halved. Lists every such copy and whether it is hooked.

usage: dupload.py <ELF> <recomp/kamite.toml> <recomp/kamite_functions.csv>
"""
import bisect, csv, struct, sys, tomllib
import rabbitizer

def main(elf, toml_path, funcs_path):
    d = open(elf, 'rb').read()
    phoff, = struct.unpack_from('<I', d, 0x1C); phnum, = struct.unpack_from('<H', d, 0x2C)
    segs = []
    for i in range(phnum):
        t, off, va, pa, fs, ms = struct.unpack_from('<6I', d, phoff + i * 32)
        if t == 1:
            segs.append((va, off, fs))
    def word(a):
        for va, off, fs in segs:
            if va <= a < va + fs:
                return struct.unpack_from('<I', d, off + a - va)[0]
        return None
    hooks = tomllib.load(open(toml_path, 'rb'))['patches']['hooks']
    hooked = {int(h['address'], 16): h for h in hooks}
    fs = sorted((int(r[1], 16), int(r[2], 16), r[0]) for r in csv.reader(open(funcs_path)) if r[1].startswith('0x'))
    starts = [f[0] for f in fs]
    missing = 0
    for a, h in sorted(hooked.items()):
        w = word(a)
        if w is None or not h.get('replace'):  # the replacing hooks (the 60 FPS countdowns); see the notes for the others
            continue
        lo, hi, name = fs[bisect.bisect_right(starts, a) - 1]
        for b in range(lo, hi, 4):
            i = rabbitizer.Instruction(word(b), vram=b, category=rabbitizer.InstrCategory.R5900)
            if not i.isBranch():
                continue
            if b + 4 + (i.getProcessedImmediate() << 2) == a + 4 and word(b + 4) == w and b + 4 != a:
                state = 'hooked' if b + 4 in hooked else 'MISSING'
                missing += state == 'MISSING'
                print(f"0x{b + 4:08X} copy of 0x{a:08X} {h['hook']} {name} {i.getOpcodeName()} {state}")
    print(missing, 'missing')

if __name__ == '__main__':
    main(*sys.argv[1:4])
