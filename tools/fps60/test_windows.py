#!/usr/bin/env python3
"""Windows in a test-mode CSV (claude/test-mode.md): for each field, the runs of frames where it is "on", as game
frames and as real time, so a 30 FPS run and a 60 FPS run compare directly.

    test_windows.py run30.csv [run60.csv] [--cols pl_b1603,pl_h1618,...]

A numeric column is on while non-zero; pl_hurt (one bit per take node, 1 = can be hit) is on while any node is off;
the shapes file is not read. Without --cols: every pl_* timer column, hitstop, attack_on and pl_hurt.
"""
import csv
import sys

DEFAULT = ["pl_b1603", "pl_h1618", "pl_h1616", "pl_w5F8", "pl_w5FC", "pl_b627", "pl_b628", "pl_h648", "pl_hurt",
           "hitstop", "attack_on", "pl_guard"]


def on(column, value):
    if value == "":
        return False
    if column == "pl_hurt":
        return "0" in value
    try:
        return float(value) != 0.0
    except ValueError:
        return value not in ("0", "00000000", "0000")


def windows(path, columns):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return {}, 2
    div = int(rows[0]["div"])
    result = {}
    for column in columns:
        if column not in rows[0]:
            continue
        runs, start = [], None
        for row in rows:
            frame = int(row["frame"])
            if on(column, row[column]):
                if start is None:
                    start = frame
            elif start is not None:
                runs.append((start, frame - start))
                start = None
        if start is not None:
            runs.append((start, int(rows[-1]["frame"]) + 1 - start))
        result[column] = runs
    return result, div


def main(argv):
    columns = DEFAULT
    files = []
    i = 0
    while i < len(argv):
        if argv[i] == "--cols":
            columns = argv[i + 1].split(",")
            i += 2
        else:
            files.append(argv[i])
            i += 1
    if not files:
        print(__doc__)
        return 1
    reports = [(path,) + windows(path, columns) for path in files]
    for column in columns:
        print(column)
        for path, result, div in reports:
            runs = result.get(column)
            if runs is None:
                print(f"  {path}: no such column")
                continue
            text = ", ".join(f"f{s} x{n} ({n * div / 60.0:.3f} s)" for s, n in runs[:12])
            more = f" ... {len(runs)} runs" if len(runs) > 12 else ""
            print(f"  {path} ({60 // div} FPS): {text or 'never on'}{more}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
