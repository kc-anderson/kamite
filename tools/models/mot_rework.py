"""Rework an NPC pose motion (MOT) in place of remaking it: slower, calmer, with a side-to-side weight shift.

    python -I tools/models/mot_rework.py <in.mot> <out.mot> [--slow 1.0] [--calm 1.0 --period 27]
        [--sway 0 --sway-cycles 4 --sway-roll 2.0] [--tolerance 0.004]

--slow     stretches the motion in time (1.5 = two thirds the speed; the loop gets longer by as much).
--calm     scales each curve's quick movement round its own slow course (a moving average over --period frames,
           the length of the movement to calm: a whole period of a box filter takes that rhythm out of the course),
           1 = as it is, 0.3 = a third of it. The slow course (looks round, the hands' drift) stays.
--sway     a weight shift: the hips (bone 0) move this far to her left and right (metres), rolling with it by
           --sway-roll degrees (the hip she leans on goes up); the waist (bone 1) rolls back by as much, so the upper
           body stays upright over the counter. --sway-cycles times round the loop (whole, so it loops cleanly).
           The feet are the leg IK's targets and stay where they are.
Times are in frames of the output (after --slow). Needs numpy and scipy (tools/models/npc_motion.py's writer).
"""
import math, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mot import Mot
from npc_motion import fit_channel, write_mot


def circular_mean(v, window):
    """Moving average over `window` frames, wrapping round the loop."""
    from scipy.ndimage import uniform_filter1d
    return uniform_filter1d(v, size=max(1, min(int(round(window)), len(v))), mode='wrap')


def main(args):
    opt = lambda k, d: (args.pop(args.index(k) + 1), args.remove(k))[0] if k in args else d
    slow = float(opt('--slow', 1.0)); calm = float(opt('--calm', 1.0)); period = float(opt('--period', 27))
    sway = float(opt('--sway', 0.0)); cycles = int(opt('--sway-cycles', 4)); roll = float(opt('--sway-roll', 2.0))
    tol = float(opt('--tolerance', 0.004))
    src, out = args[:2]
    m = Mot(open(src, 'rb').read())
    n_in = m.frame_count
    n = int(round(n_in * slow))
    curves = {}
    for r in m.records:
        if r.sentinel:
            continue
        if r.constant is not None:
            curves[(r.bone, r.component)] = (r.flags, None, r.constant)
        else:
            v = np.array([r.sample(f * n_in / n) for f in range(n)])  # looped motion: frame n wraps to 0
            curves[(r.bone, r.component)] = (r.flags, v, None)
    if calm != 1.0:
        for key, (flags, v, c) in list(curves.items()):
            if v is None:
                continue
            course = circular_mean(v, period * slow)
            curves[key] = (flags, course + calm * (v - course), None)
    if sway:
        phase = 2 * math.pi * cycles * np.arange(n) / n
        s = np.sin(phase)
        def add(key, wave):
            flags, v, c = curves.get(key, (0, None, 0.0))
            base = v if v is not None else np.full(n, c)
            curves[key] = (flags, base + wave, None)
        add((0, 0), sway * s)                       # the hips to her left (+x) and right
        add((0, 5), math.radians(roll) * s)         # rolling with it: the side she leans on goes up
        add((1, 5), -math.radians(roll) * s)        # the waist rolls back: the chest stays over the counter
    recs = []
    for (bone, comp), (flags, v, c) in curves.items():
        if v is None:
            recs.append((bone, comp, flags, ('const', c)))
        else:
            recs.append((bone, comp, flags, fit_channel(v, True, 0.002 if comp < 3 else tol)))
    recs.sort(key=lambda r: (r[0] if r[0] != 255 else -1, r[1]))
    data = write_mot(n, m.loop, recs)
    Mot(data)
    open(out, 'wb').write(data)
    print('%s: %d frames, %d bytes' % (out, n, len(data)))
    if len(data) > 0x3000:
        print('warning: over 12 KB; the port takes 16 KB with its own adjustments on top: try a larger --tolerance')


if __name__ == '__main__':
    main(sys.argv[1:])
