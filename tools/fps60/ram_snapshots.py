#!/usr/bin/env python3
"""Values anywhere in EE RAM that drain over seconds, at 30 and 60 (test_mode.cpp writeSnapshot; claude/test-mode.md).

    ram_snapshots.py cases\\2026-10-29_12-49-59.case [--secs 3]   (needs numpy; Claude runs it, not the testers)

Reads <case>.30.snap.bin and <case>.60.snap.bin (the whole RAM once a second of game time, as changed words). For
every word that falls (as float, int32 or the low int16) in each of at least --secs consecutive seconds, prints how
much it falls per second in each replay. A meter that drains twice as fast at 60 shows a ratio of ~2.
"""
import sys
import numpy as np

WORDS = 0x2000000 // 4


def snapshots(path):
    """[(frame, uint32 array of the whole RAM)] one per second."""
    data = np.fromfile(path, dtype=np.uint32)
    if data[:1].tobytes() != b"KSNP":
        raise SystemExit(f"{path}: not a snapshot file")
    ram = np.zeros(WORDS, dtype=np.uint32)
    out, at = [], 1
    while at + 2 <= data.size:
        frame, count = int(data[at]), int(data[at + 1])
        at += 2
        pairs = data[at:at + 2 * count].reshape(-1, 2)
        at += 2 * count
        ram[pairs[:, 0]] = pairs[:, 1]
        out.append((frame, ram.copy()))
    return out


def series(snaps, idx):
    m = np.stack([ram[idx] for _, ram in snaps])  # (snapshots, words)
    f = m.view(np.float32).astype(np.float64)
    i32 = m.view(np.int32).astype(np.float64)
    i16 = (m & 0xFFFF).astype(np.uint16).view(np.int16).astype(np.float64)
    return {"f": f, "w": i32, "h": i16}


def drains(col, secs):
    """(start second, seconds, drop per second) of the longest run of falling seconds, or None."""
    if not np.all(np.isfinite(col)):
        return None
    d = np.diff(col)
    best = None
    start = None
    for k in range(d.size + 1):
        if k < d.size and d[k] < 0:
            if start is None:
                start = k
        else:
            if start is not None and k - start >= secs and (best is None or k - start > best[1]):
                best = (start, k - start, (col[start] - col[k]) / (k - start))
            start = None
    return best


def main(argv):
    secs, files = 3, []
    i = 0
    while i < len(argv):
        if argv[i] == "--secs":
            secs = int(argv[i + 1]); i += 2
        else:
            files.append(argv[i]); i += 1
    if len(files) != 1:
        print(__doc__)
        return 1
    s30, s60 = snapshots(files[0] + ".30.snap.bin"), snapshots(files[0] + ".60.snap.bin")
    # Words that changed in most seconds of both runs.
    def busy(snaps):
        changes = np.zeros(WORDS, dtype=np.int32)
        for (_, a), (_, b) in zip(snaps, snaps[1:]):
            changes += (a != b)
        return changes
    c30, c60 = busy(s30), busy(s60)
    idx = np.flatnonzero((c30 >= secs) & (c60 >= secs))
    v30, v60 = series(s30, idx), series(s60, idx)
    rows = []
    for typ in ("f", "w", "h"):
        for j, word in enumerate(idx):
            a, b = drains(v30[typ][:, j], secs), drains(v60[typ][:, j], secs)
            if a is None or b is None:
                continue
            if typ == "f" and (abs(v60[typ][0, j]) > 1e6 or abs(a[2]) < 1e-6):
                continue
            ratio = b[2] / a[2] if a[2] else float("inf")
            rows.append((abs(np.log(max(ratio, 1e-9))), word * 4, typ, a, b, ratio,
                         v30[typ][a[0], j], v60[typ][b[0], j]))
    print(f"Draining values in {files[0]} ({len(s30)} / {len(s60)} seconds, {idx.size} busy words)")
    print("address    type  30: from, secs, drop/s            60: from, secs, drop/s            60/30")
    # Ratio ~2 first (double speed at 60), then ~1 (already right).
    for _, addr, typ, a, b, ratio, start30, start60 in sorted(rows, key=lambda r: abs(r[5] - 2.0))[:40]:
        print(f"0x{addr:07X} {typ}   {start30:10.4g} {a[1]:2d}s {a[2]:10.4g}/s   {start60:10.4g} {b[1]:2d}s {b[2]:10.4g}/s   {ratio:5.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
