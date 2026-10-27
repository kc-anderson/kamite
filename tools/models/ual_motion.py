"""An NPC pose motion from a clip of Quaternius' Universal Animation Library 1 or 2 (CC0): the upper body and the hips
from the clip, retargeted to the game's 30-bone woman's skeleton; the legs and feet are the bunny girl's idle
(ema5.dat motion 19), placed by the game's leg IK, so the feet stay planted whatever the clip's feet do.

    python -I tools/models/ual_motion.py <UAL1_Standard.glb | UAL2_Standard.glb> <clip> <extracted em/ema5.dat> <out.mot>
        [--slow 1.0] [--hips 1.0] [--tolerance 0.004]

Clip names as in the library (e.g. Dance_Loop in UAL1, Idle_FoldArms_Loop in UAL2). The upper body (bones 1-16, 26,
29) is retargeted as npc_rail.py does it (each bone's world rotation relative to the library's A_TPose, carried over to
the game's T-pose). The hips (bone 0) take the clip's pelvis: its rotation, with the clip's mean heading taken out (she
faces where the port's Turn says) and its mean tilt moved into the waist, and its travel round its mean, scaled to the game's hip height (1.117 m) and by
--hips. The pelvis (bone 17) stays still, so all the hip movement is bone 0's, which the port's Hip sway scales.
--slow stretches the clip (1.5 = two thirds the speed). Needs numpy and scipy.
"""
import os, sys
import numpy as np
from scipy.spatial.transform import Rotation as Rot

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import npc_rail
from gamemodel import dat_read
from mot import Mot
from npc_motion import fit_channel, write_mot

HIP_HEIGHT = 1.117  # the game's bone 0 height in its idles


def build(glb, clip, ema5, slow=1.0, hips=1.0, tol=0.004):
    npc_rail.GLB, npc_rail.EMA5 = glb, ema5
    g = npc_rail.GLB_(glb)
    bones, _ = npc_rail.game_parents()
    tpose, _ = g.anim('A_TPose')
    WRt, WPt = g.world(*g.pose_at(tpose, 0))
    tracks, times = g.anim(clip)
    dur = float(times[-1])
    n = max(2, int(round(dur * 30.0 * slow)))
    pelvis = g.byname['pelvis']
    rot = {gb: [] for gb in list(npc_rail.MAP) + [0]}
    hp = []
    for f in range(n):
        WR, WP = g.world(*g.pose_at(tracks, dur * f / n))  # looping: frame n is frame 0
        for gb, name in npc_rail.MAP.items():
            s = g.byname[name]
            rot[gb].append(WR[s] @ WRt[s].T)
        rot[0].append(WR[pelvis] @ WRt[pelvis].T)
        hp.append(WP[pelvis])
    # The clip's mean heading out of everything: she faces forward, the port's Turn sets where.
    fwd = np.mean([r @ np.array([0.0, 0.0, 1.0]) for r in rot[0]], axis=0)
    unturn = Rot.from_euler('y', -np.arctan2(fwd[0], fwd[2])).as_matrix()
    for gb in rot:
        rot[gb] = [unturn @ r for r in rot[gb]]
    hp = np.array([unturn @ p for p in hp])
    # The hips' mean tilt goes to the waist: the legs (the idle's, on the leg IK) stand under her, the upper body
    # leans as the clip's did (bone 1's local rotation takes it, as its world rotation is unchanged).
    q = np.array([Rot.from_matrix(r).as_quat() for r in rot[0]])
    q[np.dot(q, q[0]) < 0] *= -1
    tilt = Rot.from_quat(q.mean(axis=0) / np.linalg.norm(q.mean(axis=0))).as_matrix()
    rot[0] = [tilt.T @ r for r in rot[0]]
    scale = HIP_HEIGHT / max(WPt[pelvis][1], 1e-3)
    mean = hp.mean(axis=0)
    travel = (hp - mean) * scale * hips
    base = np.array([0.0, HIP_HEIGHT + (mean[1] - WPt[pelvis][1]) * scale, 0.0])
    hips_pos = travel + base
    # Local rotations in the game's order (R = Rz Ry Rx), unwrapped over time.
    local = {}
    for gb in rot:
        out = []
        for f in range(n):
            if gb == 0:
                L = rot[0][f]
            else:
                p = bones[gb]['parent']
                Wp = rot[p][f] if p in rot else (rot[0][f] if p >= 0 else np.eye(3))
                L = Wp.T @ rot[gb][f]
            out.append(Rot.from_matrix(L).as_euler('xyz'))
        local[gb] = np.unwrap(np.array(out), axis=0)
    # The legs: the bunny girl's idle (motion 19), resampled over the new length; the feet's IK targets constant.
    idle = Mot(dat_read(open(ema5, 'rb').read())[19][1])
    recs = []
    for r in idle.records:
        if r.sentinel or r.bone in npc_rail.MAP or r.bone == 0:
            continue
        if r.constant is not None:
            recs.append((r.bone, r.component, r.flags, ('const', r.constant)))
            continue
        if (r.bone in (20, 24) and r.component in (0, 2)) or r.bone == 255 or r.bone == 17:
            v = 0.0 if r.bone == 255 else float(np.mean([r.sample(f) for f in range(idle.frame_count)]))
            recs.append((r.bone, r.component, r.flags, ('const', v)))  # the pelvis still: the hips are bone 0's
            continue
        vals = np.array([r.sample(f * idle.frame_count / n) for f in range(n)])
        recs.append((r.bone, r.component, r.flags, fit_channel(vals, True, tol)))
    for c in range(3):
        recs.append((0, c, 0, fit_channel(hips_pos[:, c], True, 0.002)))
        recs.append((0, 3 + c, 0, fit_channel(local[0][:, c], True, tol)))
    for gb in sorted(npc_rail.MAP):
        for c in range(3):
            recs.append((gb, 3 + c, 0, fit_channel(local[gb][:, c], True, tol)))
    recs.sort(key=lambda r: (r[0] if r[0] != 255 else -1, r[1]))
    return write_mot(n, 1, recs), n


if __name__ == '__main__':
    args = sys.argv[1:]
    opt = lambda k, d: (args.pop(args.index(k) + 1), args.remove(k))[0] if k in args else d
    slow = float(opt('--slow', 1.0)); hips = float(opt('--hips', 1.0)); tol = float(opt('--tolerance', 0.004))
    glb, clip, ema5, out = args[:4]
    data, n = build(glb, clip, ema5, slow, hips, tol)
    Mot(data)
    open(out, 'wb').write(data)
    print('%s: %s, %d frames (%.1f s), %d bytes' % (out, clip, n, n / 30.0, len(data)))
    if len(data) > 0x3000:
        print('warning: over 12 KB; the port takes 16 KB with its own adjustments on top: try a larger --tolerance')
