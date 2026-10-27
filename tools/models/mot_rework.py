"""Rework an NPC pose motion (MOT) in place of remaking it: slower, calmer, with a side-to-side weight shift.

    python -I tools/models/mot_rework.py <in.mot> <out.mot> [--slow 1.0] [--calm 1.0 --period 27]
        [--sway 0 --sway-cycles 4 --sway-roll 2.0] [--arm-out r|l 0.085 --ema5 <extracted em/ema5.dat>] [--wrist]
        [--tolerance 0.004]

--slow     stretches the motion in time (1.5 = two thirds the speed; the loop gets longer by as much).
--calm     scales each curve's quick movement round its own slow course (a moving average over --period frames,
           the length of the movement to calm: a whole period of a box filter takes that rhythm out of the course),
           1 = as it is, 0.3 = a third of it. The slow course (looks round, the hands' drift) stays.
--sway     a weight shift: the hips (bone 0) move this far to her left and right (metres), rolling with it by
           --sway-roll degrees (the hip she leans on goes up); the waist (bone 1) rolls back by as much, so the upper
           body stays upright over the counter. --sway-cycles times round the loop (whole, so it loops cleanly).
           The feet are the leg IK's targets and stay where they are.
--arm-out  keeps that arm's elbow at least this far (metres) in front of the chest bone, swinging the upper arm
           forward round the shoulder where the motion brings it closer (a hand clipping into the chest in a
           crossed-arms idle). Needs the game's skeleton: --ema5.
--wrist    motions made before Oct 27 put the hand's rotation on the forearm's twist bone (9 / 15, mid-forearm) and
           the fingers' on the hand (10 / 16, at the wrist): the hand's rotation moves to the hand bone, the twist
           bone turns with the forearm (as the tools now make them).
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


def arm_out(curves, n, side, min_z, ema5):
    """The upper arm (bones 6+7 right, 12+13 left; parent: the clavicle) swung forward round the shoulder's side axis
    by what keeps the elbow (bone 8 / 14) min_z in front of the chest bone (2) in world z, each frame, smoothed."""
    from scipy.spatial.transform import Rotation as Rot
    import npc_rail
    npc_rail.EMA5 = ema5
    bones, _ = npc_rail.game_parents()
    clav, uppers, elbow = (5, (6, 7), 8) if side == 'r' else (11, (12, 13), 14)

    def local(f):
        loc = {}
        for (bone, comp), (flags, v, c) in curves.items():
            if bone < len(bones):
                loc.setdefault(bone, [0.0] * 6)[comp] = v[f] if v is not None else c
        return loc

    def fk(loc):
        W, P = {}, {}
        for i, b in enumerate(bones):
            l = loc.get(i, [0.0] * 6)
            R = Rot.from_euler('xyz', l[3:6]).as_matrix()
            t = np.array(l[0:3]) if i == 0 else np.array(b['t'], float)
            p = b['parent']
            if p < 0 or p not in W:
                W[i], P[i] = R, t
            else:
                W[i], P[i] = W[p] @ R, P[p] + W[p] @ t
        return W, P

    upper_len = float(np.linalg.norm(bones[elbow]['t']))
    deficit = np.zeros(n)
    frames = []
    for f in range(n):
        loc = local(f)
        W, P = fk(loc)
        deficit[f] = max(0.0, min_z - (P[elbow][2] - P[2][2]))
        frames.append((loc, W))
    angle = circular_mean(np.arctan2(deficit, upper_len), 9)
    print('arm-out %s: elbow short of %.3f m by up to %.3f; swung forward by up to %.1f deg' % (side, min_z, deficit.max(), np.degrees(angle.max())))
    new = {b: np.zeros((n, 3)) for b in uppers}
    for f in range(n):
        loc, W = frames[f]
        Wp = W[clav]
        pre = Wp.T @ Rot.from_euler('x', -angle[f]).as_matrix() @ Wp  # round the world's side axis, in the clavicle's space
        for b in uppers:
            L = Rot.from_euler('xyz', loc.get(b, [0.0] * 6)[3:6]).as_matrix()
            new[b][f] = Rot.from_matrix(pre @ L).as_euler('xyz')
    for b in uppers:
        e = np.unwrap(new[b], axis=0)
        for c in range(3):
            flags = curves.get((b, 3 + c), (0, None, 0.0))[0]
            curves[(b, 3 + c)] = (flags, e[:, c], None)


def main(args):
    opt = lambda k, d: (args.pop(args.index(k) + 1), args.remove(k))[0] if k in args else d
    slow = float(opt('--slow', 1.0)); calm = float(opt('--calm', 1.0)); period = float(opt('--period', 27))
    sway = float(opt('--sway', 0.0)); cycles = int(opt('--sway-cycles', 4)); roll = float(opt('--sway-roll', 2.0))
    tol = float(opt('--tolerance', 0.004))
    arm, arm_min = None, 0.0
    if '--arm-out' in args:
        i = args.index('--arm-out')
        arm, arm_min = args[i + 1], float(args[i + 2])
        del args[i:i + 3]
    ema5 = opt('--ema5', None)
    wrist = '--wrist' in args
    if wrist:
        args.remove('--wrist')
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
    if wrist:
        for twist, hand in ((9, 10), (15, 16)):
            for c in (3, 4, 5):
                flags = curves.get((hand, c), curves.get((twist, c), (0, None, 0.0)))[0]
                curves[(hand, c)] = (flags,) + curves.get((twist, c), (0, None, 0.0))[1:]
                curves[(twist, c)] = (flags, None, 0.0)
    if arm:
        arm_out(curves, n, arm, arm_min, ema5)
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
