"""Spanking on the human-sized men and tall bodies at enemy size 1.30 (K, Nov 12, Dr. Ion at 1.29). The game's own
placement; on a big enemy the victim's hips, belly and back rode high over Gene's thigh, so (K's decisions):
- the victim lowered until the belly rests on Gene's thigh (median of the held frames 1 cm in), the knees down to the
  floor (no deeper than 1 cm) with the feet moved back (the ankles' IK targets) as far as that needs: drop / feet,
  constant through the grab (eased in over its first 8 frames) and the spanks;
- the finish (1331 has its legs by rotation, no IK): the drop per key, only as far as the belly needs (Gene lifts the
  victim), out from frame 12 to the launch at 21; at its frame 0 the thighs / knees turned to where the spanks left
  the knees and feet (no jump at the motion switch), back to the game's by frame 4;
- the knees never cross (checked: their spread along the hips stays positive);
- Gene's spanking (right) arm: one raise / swing / elbow, held through the spanks like the left (same ease), the slaps
  landing as deep as the original pairing's on this body at 1.0 (not deeper) at the same spot;
- Gene's holding (left) arm straight across the back, palm down (spank_fat2's STRAIGHT / TWIST), raised to the lowest
  angle where the forearm clears the lowered back (median of the held frames <= 0.3 cm).
Writes fits7/g_Spanking_<g>_1.30 (.npy: Gene's keys x 14; .json: victim= the victim's keys per motion, the file's
values: height, bend, hips back, tilt, legs, feet back, thigh, knee); emit7 writes them.

    GODHAND_EXTRACT=<extract> python -I spank130.py [group ...]       (then emit7)
    GODHAND_EXTRACT=<extract> python -I spank130.py --gif <group> <yaw> <pitch> <out.gif>   (now | new; K's angle 175 8)"""
import sys, os, json, math, time, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from scipy.optimize import least_squares
import fit_pose
from fit_pose import path_of, TYPES, REP, type_of, OUT
from spank_fat2 import STRAIGHT, TWIST
import pairsim as ps
SCALE = 1.3
KS = [[0, 4, 8, 12, 16, 20, 22], [0, 4, 8, 12, 16], [0, 4, 8, 12, 16, 20, 23]]   # Gene's keys per stage
CF = [(1, 3), (1, 4), (1, 5), (1, 6), (1, 7), (1, 8), (2, 1), (2, 2), (2, 3), (2, 4)]   # around the slaps
NOW = dict(lr=75, le=45, tw=TWIST)   # the Nov 12 1.30 fit (for the review's "now")
def groups(): return [g for t in ('men', 'tall') for g in TYPES[t]]
def path(g): t = type_of(g); return path_of(g, t if g == REP[t] else None)
def held(f): return (f['stage'] == 0 and f['t'] >= 8) or f['stage'] == 1 or (f['stage'] == 2 and f['t'] <= 12)
def wfin(f): return 1.0 if f <= 12 else max(0.0, (21 - f) / 9.0)

def solve_drop_feet(b):
    """The drop that rests the belly (median of the held frames 1 cm in); for each drop, the least feet back that keeps
    the knees out of the floor (10th percentile 0.5 cm in, none past 1 cm). Never feet forward (K: no folding)."""
    def run(d, F):
        fr = ps.simulate(b.path, SCALE, ve=dict(drop=d, feet=F), stages=(0, 1)); H = [f for f in fr if held(f)]
        bl = np.array([b.belly(f) for f in H]); kf = np.array([b.knee_floor(f) for f in H])
        return np.median(bl[bl < 5]), min(np.percentile(kf, 10) + 0.005, kf.min() + 0.01)
    def feet_for(d):
        bm, k = run(d, 0.0)
        if k >= 0: return 0.0, bm
        lo, hi = 0.0, 0.4
        for _ in range(8):
            F = (lo + hi) / 2; bm, k = run(d, F)
            if k >= 0: hi = F
            else: lo = F
        return hi, run(d, hi)[0]
    lo, hi = 0.0, 0.3
    for _ in range(8):
        d = (lo + hi) / 2; F, bm = feet_for(d)
        if bm > -0.01: lo = d
        else: hi = d
    d = (lo + hi) / 2; return d, feet_for(d)[0]

def finish_legs(b, D, F):
    """Thigh / knee (rad) at the finish's frame 0 that put the knees and feet where the spanks' last frame left them."""
    P1 = ps.simulate(b.path, SCALE, ve=dict(drop=D, feet=F), want={(1, 16)}, stages=(1,), mesh=False)[-1]['vP']
    def res(x):
        P = ps.simulate(b.path, SCALE, ve=dict(keys={1331: [(0, dict(drop=D, thigh=x[0], knee=x[1]))]}), want={(2, 0)},
                        stages=(2,), mesh=False)[0]['vP']
        return np.concatenate([P[j] - P1[j] for j in (19, 23, 20, 24)])
    return least_squares(res, [0, 0], diff_step=0.05).x

FIN = [0, 1, 2, 3, 4, 6, 8, 10, 12, 16, 20]   # the finish's victim keys
RISE = 0.02   # m a frame: the most the victim's hips rise from the drop in the finish (Gene lifts; no pop)

def victim_keys(b, D, F):
    K = {1327: [(f, dict(drop=D * min(1, f / 8), feet=F * min(1, f / 8))) for f in KS[0]],
         1329: [(f, dict(drop=D, feet=F)) for f in KS[1]]}
    th0 = finish_legs(b, D, F)
    fr = ps.simulate(b.path, SCALE, ve=dict(keys={1331: [(0, {})]}), stages=(2,), want={(2, f) for f in FIN})
    gap = {f['t']: b.belly(f) for f in fr}; game = {f['t']: b.knee_floor(f) for f in fr}   # with no edits
    def knees(f, d, x):
        v = ps.simulate(b.path, SCALE, ve=dict(keys={1331: [(0, dict(drop=d, thigh=x[0], knee=x[1]))]}), stages=(2,), want={(2, f)})[0]
        return b.knee_floor(v)
    keys = []; prev, last = D, 0
    for f in FIN:
        need = min(D, max(0.0, (gap[f] + 0.01) / SCALE))
        d = D if f == 0 else max(need, prev - RISE * (f - last) / SCALE)   # the hips rise no faster than RISE
        x = th0 * max(0.0, 1 - f / 4.0); floor = min(-0.005, game[f])
        if f > 0 and knees(f, d, x) < floor - 0.005:   # the knees out of the floor: less drop (as far as needed)
            lo, hi = 0.0, d
            for _ in range(7):
                m = (lo + hi) / 2
                if knees(f, m, x) < floor - 0.005: hi = m
                else: lo = m
            d = lo
        prev, last = d, f
        keys.append((f, dict(drop=d * wfin(f), thigh=x[0], knee=x[1])))
    K[1331] = keys
    return K

def slaps(b, ve, gp):
    fr = ps.simulate(b.path, b.scale, ve=ve, gp=gp, want=set(CF), stages=(1, 2))
    return {(f['stage'], f['t']): b.hand(f) for f in fr}

def right_arm(g, K):
    ref = slaps(ps.Body(path(g), 1.0), None, None); b = ps.Body(path(g), SCALE)
    def res(x):
        x = np.asarray(x) * 10.0; h = slaps(b, dict(keys=K), dict(NOW, rr=x[0], rs=x[1], re=x[2])); r = []
        for k in CF:
            (s0, p0), (s1, p1) = ref[k], h[k]
            if s0 < 0.03:   # a slap: as deep as the original's (never > 0.5 cm deeper), at its spot
                d = s1 - s0; r.append(d / 0.005 if d > -0.005 else (d + 0.005) / 0.0015 - 1.0); r += list((p1[:2] - p0[:2]) / 0.04)
            else: r.append(min(0, s1 - min(s0, 0.0)) / 0.005)
        return np.array(r + list(np.array(x) / 40.0))
    best = min((least_squares(res, x0, diff_step=0.05, max_nfev=40, bounds=([-4] * 3, [4] * 3)) for x0 in ([0, 0, 0], [-1.5, 2, -2.5])),
               key=lambda r: r.cost)
    x = best.x * 10.0   # 10 deg units; two starts (from 0 it can stall on the distances' steps)
    return x, ref, slaps(b, dict(keys=K), dict(NOW, rr=x[0], rs=x[1], re=x[2]))

def left_arm(b, K, gp):
    def depth(r):
        fr = ps.simulate(b.path, SCALE, ve=dict(keys=K), gp=dict(gp, lr=r, ls=STRAIGHT[0], le=STRAIGHT[1], tw=TWIST), every=2)
        d = [b.lfore(f) for f in fr if held(f) and not (f['stage'] == 0 and f['t'] < 8)]
        return np.median(d), max(d)
    lo, hi = 0.0, 85.0
    for _ in range(7):
        r = (lo + hi) / 2
        if depth(r)[0] <= 0.003: hi = r
        else: lo = r
    return hi, depth(hi)

def file_keys(K):
    """Victim keys in the file's values (height = -drop, degrees)."""
    out = {}
    for m, keys in K.items():
        out[m] = [(f, [-e.get('drop', 0), 0.0, 0.0, 0.0, 0.0, e.get('feet', 0), math.degrees(e.get('thigh', 0)),
                       math.degrees(e.get('knee', 0))]) for f, e in keys]
    return out

def job(g):
    t0 = time.time(); b = ps.Body(path(g), SCALE)
    D, F = solve_drop_feet(b); K = victim_keys(b, D, F)
    x, ref, h = right_arm(g, K)
    gp = dict(rr=x[0], rs=x[1], re=x[2]); lr, clip = left_arm(b, K, gp); gp.update(lr=lr)
    fr = ps.simulate(b.path, SCALE, ve=dict(keys=K), gp=dict(gp, ls=STRAIGHT[0], le=STRAIGHT[1], tw=TWIST))
    kf = min(b.knee_floor(f) for f in fr if f['stage'] < 2 or f['t'] <= 20); ks = min(b.knee_sep(f) for f in fr)
    bl = [b.belly(f) for f in fr if held(f)]; bl = [v for v in bl if v < 5]
    X = np.zeros((sum(map(len, KS)), 14)); o = 0
    for si, fs in enumerate(KS):
        for f in fs:
            w = ps.wstage(si, f); X[o, 8:14] = np.radians([lr, STRAIGHT[0], STRAIGHT[1], x[0], x[1], x[2]]) * w; o += 1
    t = type_of(g); name = 'g_Spanking_%02d_%.2f' % (g, SCALE)
    m = dict(name='Spanking', type=t, group=g, scale=SCALE, rep=False, own_placement=True, ltwist=TWIST, ks=KS, size=1.0,
             pose_from='own (K Nov 12: the victim lowered onto the thigh, both arms refitted)',
             victim={str(k): v for k, v in file_keys(K).items()},
             metrics=dict(drop=D * SCALE, feet=F * SCALE, knee_floor=kf, knee_sep=ks, belly=[float(np.median(bl)), float(min(bl))],
                          slaps=[(str(k), float(ref[k][0]), float(h[k][0])) for k in CF], lfore=list(map(float, clip))))
    np.save(os.path.join(OUT, name + '.npy'), X); json.dump(m, open(os.path.join(OUT, name + '.json'), 'w'), indent=1, default=float)
    return ('Spanking %s g%02d at 1.30: drop %.1f cm, feet back %.1f cm; R arm %.0f/%.0f/%.0f, L raise %.0f; knees >= %.1f cm, '
            'spread >= %.1f cm; belly %.1f cm (deepest %.1f) (%.0f s)' % (t, g, D * SCALE * 100, F * SCALE * 100, x[0], x[1], x[2], lr,
            kf * 100, ks * 100, np.median(bl) * 100, min(bl) * 100, time.time() - t0))

def review_gif(g, yaw, pitch, out):
    """Now (the Nov 12 fit) | new, every frame from the grab's 6th to the launch."""
    from raster import render
    from PIL import Image, ImageDraw
    m = json.load(open(os.path.join(OUT, 'g_Spanking_%02d_1.30.json' % g)))
    K = {int(k): [(f, dict(drop=-v[0], feet=v[5], thigh=math.radians(v[6]), knee=math.radians(v[7]))) for f, v in keys]
         for k, keys in m['victim'].items()}
    X = np.load(os.path.join(OUT, 'g_Spanking_%02d_1.30.npy' % g)); a = np.degrees(X[len(KS[0])])   # a held key
    gp = dict(lr=a[8], ls=a[9], le=a[10], rr=a[11], rs=a[12], re=a[13], tw=TWIST)
    b = ps.Body(path(g), SCALE); c, s = math.cos, math.sin; Y, P = math.radians(yaw), math.radians(pitch)
    V = np.array([[1, 0, 0], [0, c(P), -s(P)], [0, s(P), c(P)]]) @ np.array([[c(Y), 0, s(Y)], [0, 1, 0], [-s(Y), 0, c(Y)]])
    GT = ps.sim.actor(ps.sim.GENE_PATH).mesh['T']
    runs = [[f for f in ps.simulate(b.path, SCALE, ve=v, gp=p) if (f['stage'] > 0 or f['t'] >= 6) and not (f['stage'] == 2 and f['t'] > 22)]
            for v, p in ((None, NOW), (dict(keys=K), gp))]
    A = np.concatenate([np.concatenate([f['gV'], f['vV']]) for r in runs for f in r[::5]]) @ V.T
    lo, hi = A[:, :2].min(0), A[:, :2].max(0); ext = ((lo + hi) / 2, (hi - lo).max() * 1.05); S = 300; frames = []
    for i in range(len(runs[0])):
        img = Image.new('RGB', (2 * S, S + 16), (255, 255, 255)); d = ImageDraw.Draw(img)
        for ci, r in enumerate(runs):
            f = r[i]; im, _ = render([(f['gV'], GT, (90, 120, 200)), (f['vV'], b.vm['T'], (220, 140, 80))], V, size=(S, S), extent=ext)
            img.paste(Image.fromarray(im), (ci * S, 16)); d.text((ci * S + 4, 2), '%s  st%d f%d' % (('now', 'new')[ci], f['stage'], f['t']), fill=(0, 0, 0))
        frames.append(img)
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=100, loop=0)

def patch(path=os.path.join(HERE, '..', '..', '..', 'resources', 'punish_fit.txt')):
    """Replace the Spanking 1.30 blocks of these bodies in the shipped file (no full extract needed; emit7 writes the same)."""
    import emit7
    from fit_pose import B
    L = open(path).read().split('\n'); out = []; i = 0; done = 0
    blocks = {}
    for g in groups():
        f = os.path.join(OUT, 'g_Spanking_%02d_1.30' % g)
        m = json.load(open(f + '.json'))
        if m.get('victim'): blocks[','.join(B[g]['keys'])] = emit7.own_block('Spanking', ','.join(B[g]['keys']), B[g]['files'][0], type_of(g), SCALE, m, np.load(f + '.npy'))
    while i < len(L):
        p = L[i].split()
        if p[:2] == ['fit', 'Spanking'] and p[2] in blocks and 'scale=1.30' in p:
            out += blocks[p[2]]; done += 1; i += 1
            while i < len(L) and L[i].split()[:1] in (['gene'], ['victim']): i += 1
            continue
        out.append(L[i]); i += 1
    open(path, 'w').write('\n'.join(out)); print('patched %d of %d blocks' % (done, len(blocks)))

if __name__ == '__main__':
    if sys.argv[1:2] == ['--patch']: patch(); sys.exit()
    if sys.argv[1:2] == ['--gif']:
        review_gif(int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]); sys.exit()
    from multiprocessing import Pool
    gs = [int(a) for a in sys.argv[1:] if a.isdigit()] or groups()
    with Pool(2) as p:
        for line in p.imap_unordered(job, gs): print(line, flush=True)
