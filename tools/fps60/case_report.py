#!/usr/bin/env python3
"""The report for a recorded test case (claude/test-mode.md).

    case_report.py cases\\2026-10-29_14-30-12.case [--tol 0.034] [--cols pl_b1603,pl_h1618,...]

Reads the case (its metadata and the live log the tester's run wrote) and the two replays next to it
(<case>.30.csv, <case>.60.csv; Replay-Cases.cmd makes them), then prints:

1. Faithful?  The replay at the case's own frame rate against the live log, frame by frame: the replay is trusted
   only when they agree (same animations, states, positions, hits). A divergence is reported with its first frame.
2. Episodes.  Each run cut into Gene's animations (runs of frames with the same motion id), with the timer windows
   inside each (as seconds from the animation's start), the frames his hurt volumes are off, and the hit stops.
3. 30 vs 60 per animation.  Episodes grouped by motion id in each run (Gene's; enemies by kind and motion over all
   slots) and compared per id: count, median duration, each timer's median window (start offset, length), hurt-off
   and hit stop. A fight's 30 replay drifts from the 60 run wherever 60 is still broken, so the runs are not paired in
   order; medians per id survive the drift. Episodes cut by the case's first or last frame are left out. A median
   differing by more than the tolerance (default one 30 FPS frame) is flagged: that is the field to fix.
4. Sequence (short cases only, <= 8 s): the old in-order pairing, and section 2's episode list.

Durations are real time: frames x (vblanks per frame) / 60.
"""
import csv
import os
import io
import sys

TIMER_COLS = ["pl_w568", "pl_w5F8", "pl_w5FC", "pl_b621", "pl_b622", "pl_b623", "pl_b624", "pl_b625", "pl_b626",
              "pl_b627", "pl_b628", "pl_h648", "pl_f15D0", "pl_b1600", "pl_b1601", "pl_b1602", "pl_b1603", "pl_h1616",
              "pl_h1618", "pl_guard"]
FAITH_COLS = ["buttons", "pl_motion", "pl_mode", "pl_phase", "pl_step", "pl_mframe", "pl_x", "pl_y", "pl_z", "pl_hp",
              "hitstop", "pl_hurt", "pl_b1603", "pl_h1618"]
EM_FAITH = ["motion", "mode", "phase", "hp", "x", "z"]


# ---- files -------------------------------------------------------------------------------------------------------
def read_case(path):
    """The sections of a .case file (bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"KAMITE-CASE 1\n"):
        raise SystemExit(f"{path}: not a case file")
    at = len(b"KAMITE-CASE 1\n")
    sections = {}
    while at < len(data):
        eol = data.index(b"\n", at)
        line = data[at:eol].decode("ascii", "replace")
        at = eol + 1
        if line == "end":
            break
        _, name, size = line.split(" ")
        size = int(size)
        sections[name] = data[at:at + size]
        at += size + 1
    return sections


def read_rows(text):
    return list(csv.DictReader(io.StringIO(text)))


def read_csv(path):
    try:
        with open(path, newline="") as f:
            return list(csv.DictReader(f))
    except FileNotFoundError:
        return None


def meta_dict(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


# ---- helpers -----------------------------------------------------------------------------------------------------
def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def on(col, v):
    if v is None or v == "":
        return False
    if col == "pl_hurt":
        return "0" in v
    n = num(v)
    return n is not None and n != 0.0


def runs_of(rows, col, lo, hi):
    """(start, length) runs of frames in [lo, hi) where col is on."""
    out, start = [], None
    for i in range(lo, hi):
        if on(col, rows[i].get(col)):
            if start is None:
                start = i
        elif start is not None:
            out.append((start, i - start))
            start = None
    if start is not None:
        out.append((start, hi - start))
    return out


def episodes(rows, col):
    """[(value, start, end)] runs of equal col values; a restart of the animation frame (pl_mframe / mframe going
    back) starts a new run too, so the same move done twice in a row counts twice."""
    out = []
    if not rows:
        return out
    fcol = col.replace("motion", "mframe")
    fcol = fcol if fcol in rows[0] else None
    start, value = 0, rows[0].get(col)
    for i in range(1, len(rows)):
        v = rows[i].get(col)
        restart = False
        if fcol:
            a, b = num(rows[i - 1].get(fcol)), num(rows[i].get(fcol))
            restart = a is not None and b is not None and b < a
        if v != value or restart:
            out.append((value, start, i))
            start, value = i, v
    out.append((value, start, len(rows)))
    return out


def seconds(frames, div):
    return frames * div / 60.0


def match_in_order(a, b, key):
    """Pairs of episodes from a and b with equal key, in order; unmatched ones listed too."""
    pairs, j, unmatched_a = [], 0, []
    used = set()
    for ea in a:
        found = None
        for k in range(j, len(b)):
            if key(b[k]) == key(ea):
                found = k
                break
        if found is None:
            unmatched_a.append(ea)
            continue
        pairs.append((ea, b[found]))
        used.add(found)
        j = found + 1
    unmatched_b = [eb for k, eb in enumerate(b) if k not in used]
    return pairs, unmatched_a, unmatched_b


# ---- the report --------------------------------------------------------------------------------------------------
def faithful(live, replay, div, enemies):
    n = min(len(live), len(replay))
    if n == 0:
        return "no overlapping frames"
    cols = list(FAITH_COLS) + [f"em{i}_{c}" for i in range(enemies) for c in EM_FAITH]
    for i in range(n):
        for c in cols:
            a, b = live[i].get(c, ""), replay[i].get(c, "")
            if a == b:
                continue
            na, nb = num(a), num(b)
            if na is not None and nb is not None and abs(na - nb) <= 1e-3:
                continue
            shifts = []
            for d in (-2, -1, 0, 1, 2):
                same = total = 0
                for k in range(n):
                    if 0 <= k + d < len(live):
                        total += 1
                        same += all(replay[k].get(x) == live[k + d].get(x) for x in ("pl_mode", "pl_phase", "pl_motion"))
                shifts.append(f"{d:+d}: {100.0 * same / max(total, 1):.0f}%")
            return (f"DIVERGES at frame {i} ({seconds(i, div):.3f} s): {c} live={a!r} replay={b!r}  "
                    f"(with the same build this means the replay is not faithful)\n"
                    f"   Gene's state matching with the replay shifted by n frames against the live run: {', '.join(shifts)}")
    return f"yes: {n} frames agree on {len(cols)} fields ({seconds(n, div):.2f} s)"


def describe_episode(rows, div, ep, cols):
    motion, lo, hi = ep
    r0 = rows[lo]
    lines = [f"  motion {motion} from f{lo} ({seconds(lo, div):.3f} s): {hi - lo} frames = {seconds(hi - lo, div):.3f} s, "
             f"mode {r0.get('pl_mode')}/{r0.get('pl_phase')}/{r0.get('pl_step')}, seq {r0.get('pl_seq')}"]
    for col in cols + ["pl_hurt", "hitstop"]:
        rs = runs_of(rows, col, lo, hi)
        if not rs:
            continue
        label = "hurt volumes OFF" if col == "pl_hurt" else col
        text = ", ".join(f"+{seconds(s - lo, div):.3f}s x{seconds(n, div):.3f}s" for s, n in rs[:6])
        if len(rs) > 6:
            text += f" ... ({len(rs)} windows)"
        lines.append(f"      {label}: {text}")
    return "\n".join(lines)


def window_summary(rows, div, ep, col):
    """(first offset s, total on s, windows) of col inside an episode."""
    _, lo, hi = ep
    rs = runs_of(rows, col, lo, hi)
    if not rs:
        return None
    return (seconds(rs[0][0] - lo, div), seconds(sum(n for _, n in rs), div), len(rs))


def compare(rows30, rows60, cols, tol, label="Gene", mcol="pl_motion"):
    out = []
    e30 = [e for e in episodes(rows30, mcol) if e[0] not in ("", None)]
    e60 = [e for e in episodes(rows60, mcol) if e[0] not in ("", None)]
    pairs, un30, un60 = match_in_order(e30, e60, key=lambda e: e[0])
    out.append(f"{label}: {len(e30)} animations at 30, {len(e60)} at 60, {len(pairs)} matched by motion id")
    flagged = 0
    for a, b in pairs:
        d30, d60 = seconds(a[2] - a[1], 2), seconds(b[2] - b[1], 1)
        bad = abs(d60 - d30) > tol
        mark = "  !!" if bad else "    "
        out.append(f"{mark} motion {a[0]}: 30 = {d30:.3f} s ({a[2] - a[1]} f, from f{a[1]})   60 = {d60:.3f} s ({b[2] - b[1]} f, from f{b[1]})"
                   + (f"   diff {d60 - d30:+.3f} s" if bad else ""))
        flagged += bad
        for col in cols + ["pl_hurt", "hitstop"]:
            w30, w60 = window_summary(rows30, 2, a, col), window_summary(rows60, 1, b, col)
            if w30 is None and w60 is None:
                continue
            name = "hurt OFF" if col == "pl_hurt" else col
            if w30 is None or w60 is None:
                out.append(f"  !!     {name}: only at {'30' if w60 is None else '60'}: {w30 or w60}")
                flagged += 1
                continue
            diff_total = w60[1] - w30[1]
            diff_start = w60[0] - w30[0]
            bad = abs(diff_total) > tol or abs(diff_start) > tol
            flagged += bad
            out.append(f"  {'!!' if bad else '  '}     {name}: 30 starts +{w30[0]:.3f}s on {w30[1]:.3f}s ({w30[2]}w)   "
                       f"60 starts +{w60[0]:.3f}s on {w60[1]:.3f}s ({w60[2]}w)"
                       + (f"   diff start {diff_start:+.3f}s total {diff_total:+.3f}s" if bad else ""))
    if un30:
        out.append("     only at 30: " + ", ".join(f"motion {e[0]} (f{e[1]}, {e[2] - e[1]} f)" for e in un30[:10]))
    if un60:
        out.append("     only at 60: " + ", ".join(f"motion {e[0]} (f{e[1]}, {e[2] - e[1]} f)" for e in un60[:10]))
    return out, flagged


def compare_enemies(rows30, rows60, enemies, tol):
    out, flagged = [], 0
    for i in range(enemies):
        mcol, kcol = f"em{i}_motion", f"em{i}_kind"
        if not any(r.get(mcol) for r in rows30):
            continue
        kind = next((r.get(kcol) for r in rows30 if r.get(kcol)), "?")
        e30 = [e for e in episodes(rows30, mcol) if e[0]]
        e60 = [e for e in episodes(rows60, mcol) if e[0]]
        pairs, un30, un60 = match_in_order(e30, e60, key=lambda e: e[0])
        out.append(f"enemy {i} (kind {kind}): {len(e30)} animations at 30, {len(e60)} at 60, {len(pairs)} matched")
        for a, b in pairs:
            d30, d60 = seconds(a[2] - a[1], 2), seconds(b[2] - b[1], 1)
            bad = abs(d60 - d30) > tol
            flagged += bad
            out.append(f"{'  !!' if bad else '    '} motion {a[0]}: 30 = {d30:.3f} s ({a[2] - a[1]} f)   60 = {d60:.3f} s ({b[2] - b[1]} f)"
                       + (f"   diff {d60 - d30:+.3f} s" if bad else ""))
            for col in ("speed", "guard"):
                c = f"em{i}_{col}"
                w30, w60 = window_summary(rows30, 2, a, c), window_summary(rows60, 1, b, c)
                if w30 and w60 and abs(w60[1] - w30[1]) > tol:
                    flagged += 1
                    out.append(f"  !!     {c}: on {w30[1]:.3f}s at 30, {w60[1]:.3f}s at 60")
        if un30:
            out.append("     only at 30: " + ", ".join(f"motion {e[0]} ({e[2] - e[1]} f)" for e in un30[:8]))
        if un60:
            out.append("     only at 60: " + ", ".join(f"motion {e[0]} ({e[2] - e[1]} f)" for e in un60[:8]))
    return out, flagged


# ---- per animation -----------------------------------------------------------------------------------------------
def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return None
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


def group_by_id(rows, mcol, key=None):
    """{id: [(lo, hi)]} of whole episodes (not cut by the run's first or last frame)."""
    out = {}
    for value, lo, hi in episodes(rows, mcol):
        if value in ("", None) or lo == 0 or hi == len(rows):
            continue
        k = key(rows[lo], value) if key else value
        out.setdefault(k, []).append((lo, hi))
    return out


def id_stats(rows, div, eps, cols):
    """count, median duration (s) and per column (episodes with it, median start, median length) for one id."""
    st = {"n": len(eps), "dur": median([seconds(hi - lo, div) for lo, hi in eps]), "cols": {}}
    for col in cols:
        starts, totals = [], []
        for lo, hi in eps:
            rs = runs_of(rows, col, lo, hi)
            if rs:
                starts.append(seconds(rs[0][0] - lo, div))
                totals.append(seconds(sum(n for _, n in rs), div))
        if starts:
            st["cols"][col] = (len(starts), median(starts), median(totals))
    return st


def compare_ids(rows30, rows60, mcol, cols, tol, key=None, label_of=str):
    """Lines for flagged ids, the list of agreeing ids, ids seen at one rate only; and the flag count."""
    g30, g60 = group_by_id(rows30, mcol, key), group_by_id(rows60, mcol, key)
    flagged_lines, ok, one_side, flagged = [], [], [], 0
    for k in sorted(set(g30) | set(g60), key=lambda x: str(x)):
        if k not in g30 or k not in g60:
            side, eps = ("30", g30[k]) if k in g30 else ("60", g60[k])
            one_side.append(f"{label_of(k)} ({side}, x{len(eps)})")
            continue
        a, b = id_stats(rows30, 2, g30[k], cols), id_stats(rows60, 1, g60[k], cols)
        notes = []
        if abs(b["dur"] - a["dur"]) > tol:
            notes.append(f"duration {a['dur']:.3f} -> {b['dur']:.3f} s ({b['dur'] - a['dur']:+.3f})")
        for col in cols:
            ca, cb = a["cols"].get(col), b["cols"].get(col)
            name = "hurt OFF" if col == "pl_hurt" else col
            if ca is None and cb is None:
                continue
            if ca is None or cb is None:
                have, n = ("30", a["n"]) if cb is None else ("60", b["n"])
                c = ca or cb
                # A window seen in only a few episodes at one rate is usually a different situation, not a bug.
                if c[0] * 2 > n:
                    notes.append(f"{name} only at {have} (in {c[0]}/{n}, +{c[1]:.3f}s x{c[2]:.3f}s)")
                continue
            ds, dl = cb[1] - ca[1], cb[2] - ca[2]
            if abs(ds) > tol or abs(dl) > tol:
                notes.append(f"{name} +{ca[1]:.3f}s x{ca[2]:.3f}s -> +{cb[1]:.3f}s x{cb[2]:.3f}s")
        head = f"{label_of(k)} (x{a['n']} at 30, x{b['n']} at 60, {a['dur']:.3f} s)"
        if notes:
            flagged += len(notes)
            flagged_lines.append(f"  !! {head}: " + "; ".join(notes))
        else:
            ok.append(f"{label_of(k)} x{a['n']}/{b['n']}")
    return flagged_lines, ok, one_side, flagged


def enemy_rows(rows, enemies):
    """One row list per enemy slot with the columns renamed to em_*, so all slots group together."""
    out = []
    for i in range(enemies):
        pre = f"em{i}_"
        if not any(r.get(pre + "motion") for r in rows):
            continue
        out.append([{c[len(pre):]: v for c, v in r.items() if c.startswith(pre)} | {"hitstop": r.get("hitstop")} for r in rows])
    return out


def compare_enemy_ids(rows30, rows60, enemies, tol):
    """Enemy episodes of every slot grouped by (kind, motion)."""
    s30, s60 = enemy_rows(rows30, enemies), enemy_rows(rows60, enemies)
    key = lambda r, m: (r.get("kind") or "?", m)
    g30, g60 = {}, {}
    # Group per slot, then merge (episodes are slot-local: a slot can change occupant mid-run).
    for slots, g, div in ((s30, g30, 2), (s60, g60, 1)):
        for rows in slots:
            for k, eps in group_by_id(rows, "motion", key).items():
                g.setdefault(k, []).extend((rows, lo, hi) for lo, hi in eps)
    flagged_lines, ok, one_side, flagged = [], [], [], 0
    for k in sorted(set(g30) | set(g60), key=str):
        label = f"kind {k[0]} motion {k[1]}"
        if k not in g30 or k not in g60:
            side, eps = ("30", g30[k]) if k in g30 else ("60", g60[k])
            one_side.append(f"{label} ({side}, x{len(eps)})")
            continue
        def stats(items, div):
            st = {"n": len(items), "dur": median([seconds(hi - lo, div) for _, lo, hi in items]), "cols": {}}
            for col in ("speed", "guard"):
                tot = [seconds(sum(n for _, n in runs_of(r, col, lo, hi)), div) for r, lo, hi in items]
                tot = [t for t in tot if t > 0]
                if tot:
                    st["cols"][col] = median(tot)
            return st
        a, b = stats(g30[k], 2), stats(g60[k], 1)
        notes = []
        if abs(b["dur"] - a["dur"]) > tol:
            notes.append(f"duration {a['dur']:.3f} -> {b['dur']:.3f} s ({b['dur'] - a['dur']:+.3f})")
        for col in ("speed", "guard"):
            ca, cb = a["cols"].get(col), b["cols"].get(col)
            if ca is not None and cb is not None and abs(cb - ca) > tol:
                notes.append(f"{col} on {ca:.3f} -> {cb:.3f} s")
        head = f"{label} (x{a['n']} at 30, x{b['n']} at 60, {a['dur']:.3f} s)"
        if notes:
            flagged += len(notes)
            flagged_lines.append(f"  !! {head}: " + "; ".join(notes))
        else:
            ok.append(f"{k[1]} x{a['n']}/{b['n']}")
    return flagged_lines, ok, one_side, flagged


def main(argv):
    tol, cols, files = 1.0 / 30.0 + 1e-6, TIMER_COLS, []
    i = 0
    while i < len(argv):
        if argv[i] == "--tol":
            tol = float(argv[i + 1])
            i += 2
        elif argv[i] == "--cols":
            cols = argv[i + 1].split(",")
            i += 2
        else:
            files.append(argv[i])
            i += 1
    if len(files) != 1:
        print(__doc__)
        return 1
    path = files[0]
    sections = read_case(path)
    meta = meta_dict(sections.get("meta", b"").decode("utf-8", "replace"))
    live = read_rows(sections.get("live", b"").decode("utf-8", "replace"))
    rows30, rows60 = read_csv(path + ".30.csv"), read_csv(path + ".60.csv")
    rec_div = int(meta.get("divider", "2"))
    enemies = int(meta.get("enemies", "4"))

    print(f"Case {path}")
    print(f"  created {meta.get('created', '?')}, recorded at {60 // rec_div} FPS, {meta.get('frames', '?')} frames, {meta.get('polls', '?')} pad polls = "
          f"{meta.get('seconds', '?')} s, room {meta.get('room', '?')}; live log {len(live)} rows; commit {meta.get('commit', '?')}")
    if meta.get("cheats"):
        print(f"  F2 switches: {meta['cheats']}")
    settings = [f"{k[4:]}={v}" for k, v in meta.items() if k.startswith("env.")]
    if settings:
        print(f"  recorder's settings (the replays use them): {' '.join(settings)}")
    print(f"  replays: 30 FPS {'%d rows' % len(rows30) if rows30 else 'MISSING'}, 60 FPS {'%d rows' % len(rows60) if rows60 else 'MISSING'}")
    print()

    same_rate = rows60 if rec_div == 1 else rows30
    print(f"1. Faithful? (the {60 // rec_div} FPS replay against what the tester saw)")
    # Cases from before the nearest-enemy columns (meta enemyorder=near) logged cEmManage's first enemies live: their em
    # columns cannot be held against a replay's, so only Gene's are.
    faith_enemies = enemies if meta.get("enemyorder") == "near" else 0
    verdict = faithful(live, same_rate, rec_div, faith_enemies) if same_rate else "no replay at the case's rate yet"
    # Same build? (kamite.exe next to cases\, where Replay-Cases.cmd runs this.)
    recorded = meta.get("build", "")
    exe = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(path))), "kamite.exe")
    same_build = None
    if recorded and os.path.exists(exe):
        size = recorded.split(":")[0]
        same_build = size == str(os.path.getsize(exe))  # the size changes with nearly every build; the time is kept for reading
    if verdict.startswith("DIVERGES") and same_build is not True:
        verdict = ("recorded with an earlier build " + ("" if same_build is False else "(or unknown) ") +
                   "- a divergence here is expected where a fix changed the game; trust the comparison below.\n   " + verdict)
    print("   " + verdict)
    print()

    if rows30 is None or rows60 is None:
        print("Both replays are needed for the comparison.")
        return 2

    short = seconds(len(rows30), 2) <= 8.0
    total = 0
    print(f"2. 30 vs 60 per animation (median per motion id; tolerance {tol:.3f} s; !! = differs)")
    fl, ok, one, n = compare_ids(rows30, rows60, "pl_motion", cols + ["pl_hurt", "hitstop"], tol,
                                 label_of=lambda m: f"Gene motion {m}")
    total += n
    print(f"  Gene: {len(fl)} motion(s) flagged, {len(ok)} agree")
    print("\n".join(fl) if fl else "     (none flagged)")
    if ok:
        print("     agree: " + ", ".join(o.replace("Gene motion ", "") for o in ok))
    if one:
        print("     one rate only (a drifted fight, not a bug by itself): " + ", ".join(o.replace("Gene motion ", "") for o in one[:16])
              + (f" ... (+{len(one) - 16})" if len(one) > 16 else ""))
    fl, ok, one, n = compare_enemy_ids(rows30, rows60, enemies, tol)
    total += n
    print(f"  Enemies: {len(fl)} motion(s) flagged, {len(ok)} agree")
    if fl:
        print("\n".join(fl))
    if ok:
        print("     agree: " + ", ".join(ok))
    if one:
        print("     one rate only: " + ", ".join(one[:16]) + (f" ... (+{len(one) - 16})" if len(one) > 16 else ""))
    print()

    if short:
        print("3. Episodes (short case)")
        for label, rows, div in (("30 FPS", rows30, 2), ("60 FPS", rows60, 1)):
            print(f"  [{label}] {len(rows)} frames = {seconds(len(rows), div):.2f} s")
            for ep in episodes(rows, "pl_motion"):
                if ep[0] in ("", None):
                    continue
                print(describe_episode(rows, div, ep, cols))
        print()
        print(f"4. 30 vs 60 in sequence (short case)")
        lines, flagged = compare(rows30, rows60, cols, tol)
        print("\n".join(lines))
        elines, eflagged = compare_enemies(rows30, rows60, enemies, tol)
        print("\n".join(elines))
        print()

    print(f"Flagged per animation: {total} difference(s)." if total else "No per-animation differences above the tolerance.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
