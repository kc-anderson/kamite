"""A punish pair played with the runtime's motion edits (punish_fit.cpp rewrite()), for fits that change more than
heights and Gene's arms: the victim's legs (feet back, thigh / knee rotation) and pelvis. Spanking only for now
(spank130.py). Model units: the victim's values are in its model's metres (world = x its scale).

Victim edits (keys per motion: [(frame, dict)], linear between keys like the runtime's tracks):
  drop (m, down; the file's height = -drop), bend / tilt / thigh / knee (rad), back (hips back, m), feet (feet back:
  the ankles' IK targets 20 / 24 z, m; only motions with those channels, as the runtime).
Gene's pose (deg, the runtime's pre-rotations; weighted by wstage): lr/ls/le left raise / swing / elbow, rr/rs/re right,
tz/tx torso, tw left forearm twist."""
import os, math, numpy as np
import fk, sim, scene
from fk import Motion
from scipy.spatial.transform import Rotation as Rot
from punishes import PUNISH

def wstage(si, k):
    """Spanking's ease (spank_fat2.key_weights): in over the grab's first 8 frames, out from frame 12 of the finish to
    the launch at 21."""
    if si == 0: return min(1.0, k / 8.0)
    if si == 1: return 1.0
    return 1.0 if k <= 12 else max(0.0, (21 - k) / 9.0)

def interp(keys, t):
    if not keys: return None
    fs = [k[0] for k in keys]
    if t <= fs[0]: return keys[0][1]
    if t >= fs[-1]: return keys[-1][1]
    i = next(i for i, f in enumerate(fs) if f >= t); a, b = keys[i - 1], keys[i]; u = (t - a[0]) / (b[0] - a[0])
    return {n: a[1].get(n, 0) + (b[1].get(n, 0) - a[1].get(n, 0)) * u for n in set(a[1]) | set(b[1])}

class VEd:
    """The victim's motion with edits: e = dict(keys={motion: keys}) or constant values weighted by wstage."""
    def __init__(s, idx, si, off, e):
        s.m = Motion(idx); s.idx = idx; s.frames = s.m.frames; s.loop = s.m.loop; s.si = si; s.off = off; s.e = e
    def has(s, b, c): return s.m.has(b, c)
    def get(s, b, c, t, default=0.0):
        v = s.m.get(b, c, t, default)
        if 'keys' in s.e:
            e = interp(s.e['keys'].get(s.idx), t); w = 1.0 if e else 0.0
        else:
            w = wstage(s.si, s.off + t) if s.idx not in (1333, 1335) else 0.0; e = s.e
        if w == 0: return v
        if (b, c) == (0, 1): v -= w * e.get('drop', 0)
        if (b, c) == (0, 2): v -= w * e.get('back', 0)
        if (b, c) == (1, 3): v += w * e.get('bend', 0)
        if (b, c) == (17, 3): v += w * e.get('tilt', 0)
        if b in (20, 24) and c == 2 and s.m.has(20, 2): v -= w * e.get('feet', 0)
        if b in (18, 22) and c == 3: v += w * e.get('thigh', 0)
        if b in (19, 23) and c == 3: v += w * e.get('knee', 0)
        return v

def R(e): return Rot.from_euler('xyz', e).as_matrix()
def rz(a): return R([0, 0, a])
def ry(a): return R([0, a, 0])
def rx(a): return R([a, 0, 0])

class GEd:
    """Gene's motion with the runtime's pre-rotations (and the forearm twist's post-rotation), weighted by wstage."""
    BONES = (1, 13, 12, 7, 6, 14, 8, 15, 16)
    def __init__(s, idx, si, p):
        s.m = Motion(idx); s.idx = idx; s.frames = s.m.frames; s.loop = s.m.loop; s.si = si; s.p = p; s.c = {}
    def has(s, b, c): return s.m.has(b, c) or (b in s.BONES and c >= 3)
    def get(s, b, c, t, default=0.0):
        if c < 3 or b not in s.BONES: return s.m.get(b, c, t, default)
        if (b, t) not in s.c:
            w = wstage(s.si, t); p = s.p
            a = lambda n: math.radians(p.get(n, 0.0)) * w
            base = R([s.m.get(b, 3 + i, t) for i in range(3)])
            pre = {1: rz(a('tz')) @ rx(a('tx')), 13: rz(a('lr')) @ ry(a('ls')), 12: rz(a('lr') / 2) @ ry(a('ls') / 2),
                   7: rz(-a('rr')) @ ry(-a('rs')), 6: rz(-a('rr') / 2) @ ry(-a('rs') / 2), 14: ry(a('le')), 8: ry(-a('re'))}
            M = pre.get(b, np.eye(3)) @ base
            if b in (15, 16): M = base @ rx(a('tw') / 2)
            s.c[(b, t)] = Rot.from_matrix(M).as_euler('xyz')
        return s.c[(b, t)][c - 3]

def simulate(vpath, scale, ve=None, gp=None, every=1, mesh=True, stages=(0, 1, 2), want=None, name='Spanking'):
    """The pair with the game's placement. want: a set of (stage, frame) to pose (others skipped)."""
    gene = sim.actor(sim.GENE_PATH); vic = sim.actor(vpath, scale)
    gene.pos = np.array([0, 0, 1.2]); gene.yaw = math.pi; vic.pos = np.zeros(3); vic.yaw = 0.0
    gt = scene.Track(gene); vt = scene.Track(vic); out = []
    for si, stg in enumerate(PUNISH[name]):
        al = stg.get('align')
        if al: scene.align(gene, vic, (al[0], al[1]), al[2], al[3], {})
        offs = 0; vms = []
        for m in stg['v']: vms.append(VEd(m, si, offs, ve or {})); offs += Motion(m).frames - 1
        gt.start([GEd(m, si, gp or {}) for m in stg['g']]); vt.start(vms)
        total = stg.get('frames') or sum(Motion(m).frames for m in stg['g'])
        for k in range(total):
            if k > 0: gt.advance(); vt.advance()
            if k % every or si not in stages or (want is not None and (si, k) not in want): continue
            Wg, Pg = gt.pose(); Wv, Pv = vt.pose()
            rec = dict(stage=si, t=k, gP=np.array([gene.world(p) for p in Pg]), vP=np.array([vic.world(p) for p in Pv]))
            if mesh:
                rec['gV'] = gene.world(sim.skin(gene.mesh, Wg, Pg)); rec['vV'] = vic.world(sim.skin(vic.mesh, Wv, Pv))
            out.append(rec)
    return out

# Measures
def dom(mesh):
    J = np.asarray(mesh['J']); W = np.asarray(mesh['W'])
    return J if J.ndim == 1 else J[np.arange(len(J)), W.argmax(1)]
_gene = None
def gene_mesh():
    global _gene
    if _gene is None:
        m = sim.actor(sim.GENE_PATH).mesh; d = dom(m); T = np.asarray(m['T'])
        _gene = dict(T=T, d=d, thighv=np.nonzero(np.isin(d, [18, 22]))[0], hand=np.nonzero(d == 10)[0],
                     lfore=np.nonzero(np.isin(d, [14, 15, 16]))[0])
    return _gene
def sdist(Q, V, T):
    import igl
    return igl.signed_distance(Q, V, np.asarray(T), sign_type=igl.SIGNED_DISTANCE_TYPE_FAST_WINDING_NUMBER)[:3]

class Body:
    def __init__(s, path, scale):
        s.path = path; s.scale = scale; s.vm = sim.actor(path).mesh; s.d = dom(s.vm)
        s.belly_ids = np.nonzero(np.isin(s.d, [0, 1, 2, 17]))[0]
        T = np.asarray(s.vm['T']); s.belly_face = np.isin(s.d[T[:, 0]], [0, 1, 2, 17])
        s.knee_ids = [np.nonzero(np.isin(s.d, [18, 19]))[0], np.nonzero(np.isin(s.d, [22, 23]))[0]]
    def belly(s, f):
        """Gene's thighs against the victim's hips / waist / chest: the deepest thigh point in them (- = in, m) or, with
        none in, the gap between the two."""
        g = gene_mesh(); Q = f['gV'][g['thighv']]
        S, I, _ = sdist(Q, f['vV'], s.vm['T']); m = s.belly_face[I]
        if m.any() and S[m].min() < 0: return S[m].min()
        from scipy.spatial import cKDTree
        return cKDTree(f['vV'][s.belly_ids]).query(Q)[0].min()
    def knee_floor(s, f):
        """The lowest point of either knee (the leg's vertices within 10 cm of the knee joint, model units)."""
        out = []
        for j, ids in zip((19, 23), s.knee_ids):
            V = f['vV'][ids]; m = np.linalg.norm(V - f['vP'][j], axis=1) < 0.10 * s.scale
            out.append(V[m][:, 1].min() if m.any() else 9.0)
        return min(out)
    def knee_sep(s, f):
        """The knees' spread along the hips' axis (+ = the legs' own sides; <= 0 = crossed)."""
        ax = f['vP'][18] - f['vP'][22]; ax /= np.linalg.norm(ax)
        return float(np.dot(f['vP'][19] - f['vP'][23], ax))
    def hand(s, f):
        """Gene's right hand: the deepest signed distance into the victim and that spot (the hips' frame, model units)."""
        g = gene_mesh(); S, I, C = sdist(f['gV'][g['hand']], f['vV'], s.vm['T']); j = S.argmin()
        return S[j], s.local(f, C[j])
    def lfore(s, f):
        g = gene_mesh(); S, _, _ = sdist(f['gV'][g['lfore']], f['vV'], s.vm['T']); return -S.min()
    def local(s, f, p):
        o = f['vP'][0]; up = f['vP'][1] - o; up /= np.linalg.norm(up)
        sd = f['vP'][22] - f['vP'][18]; sd -= up * np.dot(sd, up); sd /= np.linalg.norm(sd)
        return np.array([np.dot(p - o, sd), np.dot(p - o, up), np.dot(p - o, np.cross(sd, up))]) / s.scale
