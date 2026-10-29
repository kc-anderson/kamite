#!/usr/bin/env python3
"""Fields that count game frames, from a case's two RAM dumps (test_mode.cpp writeRamFrame; claude/test-mode.md).

    ram_counters.py cases\\2026-10-29_11-33-11.case [--min 8] [--top 60]   (needs numpy; Claude runs it, not the testers)

Reads <case>.30.ram.bin and <case>.60.ram.bin (a record a replay frame: u32 frame, Gene's object 0x1800 bytes,
GameWork 0x800, the save data 0x200). Every aligned byte, halfword, word and float is scanned for runs where it
changes by the same step every game frame (at least --min frames). A field that steps by the same amount per game
frame at 30 and at 60 runs twice as fast in real time at 60: listed first. A field whose step at 60 is half the 30
one is already compensated. Fields counting at one rate only are listed last (situations that drift).
"""
import sys
import numpy as np

REGIONS = [("pl", 0x1800), ("gw", 0x800), ("save", 0x200)]
RECORD = 4 + sum(size for _, size in REGIONS)


def load(path):
    raw = np.fromfile(path, dtype=np.uint8)
    n = raw.size // RECORD
    return raw[: n * RECORD].reshape(n, RECORD)


def views(frames):
    """(region, offset, type) -> 1-D array per frame."""
    out = {}
    at = 4
    for name, size in REGIONS:
        block = np.ascontiguousarray(frames[:, at:at + size])
        for typ, width, dt in (("b", 1, np.uint8), ("h", 2, np.int16), ("w", 4, np.int32), ("f", 4, np.float32)):
            arr = block.view(dt)
            for k in range(arr.shape[1]):
                out[(name, k * width, typ)] = arr[:, k].astype(np.float64)
        at += size
    return out


def runs(values, min_len):
    """[(step, length)] runs where the per-frame difference is constant and non-zero."""
    if values.size < min_len + 1:
        return []
    with np.errstate(invalid="ignore", over="ignore"):
        d = np.diff(values)
    if not np.any(d):
        return []
    tol = 0.02 * np.abs(d[:-1])  # float32 rounding wobbles a step a little
    with np.errstate(invalid="ignore"):
        same = np.abs(d[1:] - d[:-1]) <= tol
    cuts = np.flatnonzero(~same) + 1               # a new run starts at these indices of d
    starts = np.concatenate(([0], cuts))
    ends = np.concatenate((cuts, [d.size]))
    lengths = ends - starts
    out = []
    for st, n in zip(starts[lengths >= min_len], lengths[lengths >= min_len]):
        step = d[st]
        if step != 0 and np.isfinite(step) and abs(step) < 1e6:
            out.append((float(f"{step:.3g}"), int(n)))
    return out


def summarise(rs):
    """The step seen most (by frames covered), its longest run, the frames covered."""
    if not rs:
        return None
    by = {}
    for step, n in rs:
        tot, longest = by.get(step, (0, 0))
        by[step] = (tot + n, max(longest, n))
    step, (tot, longest) = max(by.items(), key=lambda kv: kv[1][0])
    return step, longest, tot


def main(argv):
    min_len, top, files = 8, 60, []
    i = 0
    while i < len(argv):
        if argv[i] == "--min":
            min_len = int(argv[i + 1]); i += 2
        elif argv[i] == "--top":
            top = int(argv[i + 1]); i += 2
        else:
            files.append(argv[i]); i += 1
    if len(files) != 1:
        print(__doc__)
        return 1
    case = files[0]
    v30, v60 = views(load(case + ".30.ram.bin")), views(load(case + ".60.ram.bin"))
    double, fixed, one = [], [], []
    for key in v30:
        name, off, typ = key
        if typ == "f" and (np.isnan(v30[key]).all() or np.isnan(v60[key]).all()):
            continue
        s30, s60 = summarise(runs(v30[key], min_len)), summarise(runs(v60[key], min_len))
        if s30 is None and s60 is None:
            continue
        # Integer views of the same bytes repeat a float's runs; a byte inside a counting word repeats the word's.
        label = f"{name}+0x{off:X} {typ}"
        if s30 and s60:
            if abs(s60[0] - s30[0]) <= 0.03 * abs(s30[0]):
                double.append((s30[2] + s60[2], label, s30, s60))
            elif abs(2 * s60[0] - s30[0]) <= 0.03 * abs(s30[0]):
                fixed.append((s30[2] + s60[2], label, s30, s60))
        else:
            one.append(((s30 or s60)[2], label, s30, s60))
    # A float that counts shows up again as the word, halfwords and bytes holding it: keep the float.
    floats = {(l.split()[0].split("+")[0], int(l.split()[0].split("+0x")[1], 16)) for lst in (double, fixed, one)
              for _, l, _, _ in lst if l.endswith(" f")}
    def alias(label):
        region, rest = label.split("+0x")
        off = int(rest.split()[0], 16)
        return not label.endswith(" f") and (region, off & ~3) in floats
    double, fixed, one = ([e for e in lst if not alias(e[1])] for lst in (double, fixed, one))
    fmt = lambda s: "-" if s is None else f"step {s[0]:+g} x{s[1]} (in {s[2]} f)"
    print(f"Counters in {case} (runs of >= {min_len} frames with a constant step)")
    print(f"\nSame step per game frame at 30 and 60 = twice as fast at 60 ({len(double)}; most frames first):")
    for _, label, a, b in sorted(double, reverse=True)[:top]:
        print(f"  {label:18s} 30: {fmt(a):34s} 60: {fmt(b)}")
    print(f"\nHalf the step at 60 = compensated ({len(fixed)}):")
    for _, label, a, b in sorted(fixed, reverse=True)[:top // 2]:
        print(f"  {label:18s} 30: {fmt(a):34s} 60: {fmt(b)}")
    print(f"\nCounting at one rate only ({len(one)}):")
    for _, label, a, b in sorted(one, reverse=True)[:top // 3]:
        print(f"  {label:18s} 30: {fmt(a):34s} 60: {fmt(b)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
