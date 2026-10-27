"""An NPC pose motion from any Blender-readable animation (.blend, .fbx such as Mixamo's, .glb): retargets one
armature's action to the game's 30-bone woman's skeleton and writes a MOT for mods/npc_motions/ (npc_poses.cpp).

    python -I tools/models/npc_motion.py <animation file> <rig: custom|mixamo|ual|biped> <extracted em/ema5.dat> <out.mot>
        [--slow 1.5] [--smooth 2] [--frames a:b] [--tolerance 0.004] [--pelvis 1.0] [--head-forward]

Needs Blender's Python module (pip install bpy), numpy and scipy. The upper body, the hips and the pelvis's wiggle
come from the animation; the legs and feet are the bunny girl's idle (ema5.dat motion 19), placed by the game's leg
IK, so any bend of the source's pelvis becomes the waist's (the port's "Bottom out" tips the pelvis back). Each
bone's world rotation relative to the source's rest pose is carried over to the game's T-pose, with the rest bone
directions aligned (an A-pose rest works). --slow stretches it (1.5 = two thirds the speed), --smooth is a Gaussian
over the rotations in frames, --tolerance the curves' largest error (radians; larger = a smaller file), --pelvis scales
the hips' travel and the pelvis's wiggle, --head-forward takes out the neck's and head's mean turn. Left/right follow the source's own (the game's bones 5-10 are her right side). A rig with other axes or units
than the game's (Y up, +Z forward, metres) gives its basis and unit (biped: 3ds Max Biped).
Only motions you may share go into the repo; the rest stay in your own mods folder.
"""
import math, os, struct, sys
import numpy as np
from scipy.spatial.transform import Rotation as Rot, Slerp
from scipy.ndimage import gaussian_filter1d
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gamemodel import dat_read, scr_read, mdb_read
from mot import Mot, half, hermite

RIGS = {
 'custom': dict(fmt='b__%s__', bones={0:('ROOT_bind',None,None),1:('Spine0','Spine2',2),2:('Spine2','Neck',3),3:('Neck','Head',4),4:('Head',None,None),
   5:('R_Clavicle','R_UpperArm',7),6:('R_UpperArm','R_Forearm',8),7:('R_UpperArm','R_Forearm',8),8:('R_Forearm','R_Hand',9),9:('R_Hand','R_Mid0',10),10:('R_Mid0','R_Mid1',None),
   11:('L_Clavicle','L_UpperArm',13),12:('L_UpperArm','L_Forearm',14),13:('L_UpperArm','L_Forearm',14),14:('L_Forearm','L_Hand',15),15:('L_Hand','L_Mid0',16),16:('L_Mid0','L_Mid1',None),
   17:('Pelvis',None,None),26:('Head',None,None),29:('Spine2',None,None)}),
 'mixamo': dict(fmt='mixamorig:%s', bones={0:('Hips',None,None),1:('Spine','Spine2',2),2:('Spine2','Neck',3),3:('Neck','Head',4),4:('Head',None,None),
   5:('RightShoulder','RightArm',7),6:('RightArm','RightForeArm',8),7:('RightArm','RightForeArm',8),8:('RightForeArm','RightHand',9),9:('RightHand','RightHandMiddle1',10),10:('RightHandMiddle1','RightHandMiddle2',None),
   11:('LeftShoulder','LeftArm',13),12:('LeftArm','LeftForeArm',14),13:('LeftArm','LeftForeArm',14),14:('LeftForeArm','LeftHand',15),15:('LeftHand','LeftHandMiddle1',16),16:('LeftHandMiddle1','LeftHandMiddle2',None),
   17:('Hips',None,None),26:('Head',None,None),29:('Spine2',None,None)}),
 # 3ds Max Biped (Denys Almaral's Free Animation Pack for City People): Z up, +X forward, +Y her left, centimetres.
 'biped': dict(fmt='bip %s', basis=[[0,1,0],[0,0,1],[1,0,0]], unit=0.0119, bones={0:('Pelvis',None,None),1:('Spine','Spine1',2),2:('Spine1','Neck',3),3:('Neck','Head',4),4:('Head',None,None),
   5:('R Clavicle','R UpperArm',7),6:('R UpperArm','R Forearm',8),7:('R UpperArm','R Forearm',8),8:('R Forearm','R Hand',9),9:('R Hand','R Finger1',10),10:('R Finger1','R Finger11',None),
   11:('L Clavicle','L UpperArm',13),12:('L UpperArm','L Forearm',14),13:('L UpperArm','L Forearm',14),14:('L Forearm','L Hand',15),15:('L Hand','L Finger1',16),16:('L Finger1','L Finger11',None),
   17:('Pelvis',None,None),26:('Head',None,None),29:('Spine1',None,None)}),
 'ual': dict(fmt='%s', bones={0:('pelvis',None,None),1:('spine_01','spine_03',2),2:('spine_03','neck_01',3),3:('neck_01','Head',4),4:('Head',None,None),
   5:('clavicle_r','upperarm_r',7),6:('upperarm_r','lowerarm_r',8),7:('upperarm_r','lowerarm_r',8),8:('lowerarm_r','hand_r',9),9:('hand_r','middle_01_r',10),10:('middle_01_r','middle_02_r',None),
   11:('clavicle_l','upperarm_l',13),12:('upperarm_l','lowerarm_l',14),13:('upperarm_l','lowerarm_l',14),14:('lowerarm_l','hand_l',15),15:('hand_l','middle_01_l',16),16:('middle_01_l','middle_02_l',None),
   17:('pelvis',None,None),26:('Head',None,None),29:('spine_03',None,None)}),
}

def dump(path, frames=None):
    """Bone matrices in the armature's space: rest and every frame (Blender, Y up after the armature's own axes)."""
    import bpy
    ext = os.path.splitext(path)[1].lower()
    if ext == '.blend':
        bpy.ops.wm.open_mainfile(filepath=path)
    else:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        (bpy.ops.import_scene.fbx if ext == '.fbx' else bpy.ops.import_scene.gltf)(filepath=path)
    arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
    names = [b.name for b in arm.data.bones]
    sc = bpy.context.scene
    act = arm.animation_data.action if arm.animation_data and arm.animation_data.action else None
    f0, f1 = (int(act.frame_range[0]), int(act.frame_range[1])) if act else (sc.frame_start, sc.frame_end)
    if frames: f0, f1 = frames
    rest = np.array([np.array(arm.data.bones[n].matrix_local) for n in names])
    anim = []
    for f in range(f0, f1 + 1):
        sc.frame_set(f)
        anim.append(np.array([np.array(arm.pose.bones[n].matrix) for n in names]))
    # The bone matrices are in the armature's own space: the custom rig's is Y up, +Z forward, like the game's.
    # TODO: an armature with other axes (a Z-up Mixamo import) needs a basis change here.
    return names, rest, np.array(anim), sc.render.fps

def game_skeleton(ema5):
    d = dat_read(open(ema5, 'rb').read())[15][1]
    kind, nodes = scr_read(d)
    return mdb_read(d, nodes[0]['mdb_off'])['bones']

# --- retarget ---
def align(a,b):
    a=a/np.linalg.norm(a); b=b/np.linalg.norm(b); v=np.cross(a,b); c=np.dot(a,b)
    if np.linalg.norm(v)<1e-8: return np.eye(3)
    return Rot.from_rotvec(v/np.linalg.norm(v)*np.arccos(np.clip(c,-1,1))).as_matrix()
def retarget(names, rest, anim, rig, bones, slow=1.5, sigma=2.0):
    ix={n:i for i,n in enumerate(names)}; B=lambda s: rig['fmt']%s; MAP=rig['bones']
    gpos=[]
    for i,b in enumerate(bones):
        p=b['parent']; gpos.append(np.array(b['t'])+(gpos[p] if 0<=p<i else 0))
    A={}
    for g,(s,sc,gc) in MAP.items():
        if sc is None: A[g]=np.eye(3); continue
        sd=rest[ix[B(sc)]][:3,3]-rest[ix[B(s)]][:3,3]
        gd=np.array([-1.0 if g<11 else 1.0,0,0]) if gc is None else gpos[gc]-gpos[g]
        A[g]=align(gd,sd)
    n=len(anim)
    # smooth source rotations (quaternion components with sign continuity) and the hips position
    W={}
    for g,(s,sc,gc) in MAP.items():
        Rr=rest[ix[B(s)]][:3,:3]
        q=np.array([Rot.from_matrix(anim[f][ix[B(s)]][:3,:3]@Rr.T@A[g]).as_quat() for f in range(n)])
        for f in range(1,n):
            if np.dot(q[f],q[f-1])<0: q[f]=-q[f]
        q=gaussian_filter1d(q,sigma,axis=0,mode='wrap'); q/=np.linalg.norm(q,axis=1)[:,None]
        W[g]=q
    hip=B(MAP[0][0]); unit=rig.get('unit',1.0)   # the source's hips bone; its units to metres
    hp=np.array([anim[f][ix[hip]][:3,3] for f in range(n)])*unit; hp=gaussian_filter1d(hp,sigma,axis=0,mode='wrap')
    hp-=hp.mean(0); hp[:,1]+= (np.array([anim[f][ix[hip]][1,3] for f in range(n)]).mean()-rest[ix[hip]][1,3])*unit
    m=int(round(n*slow)); out=[]
    ts=np.arange(n+1)
    sl={g:Slerp(ts,Rot.from_quat(np.vstack([W[g],W[g][:1]]))) for g in W}
    for k in range(m):
        t=k*n/m; Wm={g:sl[g](t).as_matrix() for g in W}
        loc={}
        for g in W:
            p=bones[g]['parent']; Wp=Wm.get(p,np.eye(3)) if p>=0 else np.eye(3)
            loc[g]=Rot.from_matrix(Wp.T@Wm[g]).as_euler('xyz')
        i0=int(t)%n; fr=t-int(t); h=hp[i0]*(1-fr)+hp[(i0+1)%n]*fr
        out.append((loc,h))
    for g in out[0][0]:
        arr=np.unwrap(np.array([o[0][g] for o in out]),axis=0)
        for f,o in enumerate(out): o[0][g]=arr[f]
    return out

# --- MOT writer ---
def tohalf(x):
    """PlatinumGames half (sign 1, exp 6 bias 47, mantissa 9), round to nearest; returns (bits, decoded)."""
    if x==0 or abs(x)<2.0**-45: return 0,0.0   # below the format's smallest step
    best=None
    s=1 if x<0 else 0; a=abs(x)
    e=math.floor(math.log2(a)); 
    for ee in (e-1,e,e+1):
        E=ee+47
        if E<=0 or E>=63: continue
        m=round((a/2**ee-1)*512)
        for mm in (m-1,m,m+1):
            if mm<0 or mm>511: continue
            bits=(s<<15)|(E<<9)|mm; v=half(bits)
            if best is None or abs(v-x)<abs(best[1]-x): best=(bits,v)
    return best
def half_floor(x):
    b,v=tohalf(x)
    k=0
    while v>x and k<64:
        x2=x-abs(x)*2**-10-1e-12; b,v=tohalf(x2); x=x2; k+=1
    return b,v
def half_ceil(x):
    b,v=tohalf(x)
    k=0
    while v<x and k<64:
        x2=x+abs(x)*2**-10+1e-12; b,v=tohalf(x2); x=x2; k+=1
    return b,v
def qrange(vals):
    """pos, delta (halves) so pos + delta*k (k 0..255) covers vals; returns encoder."""
    lo,hi=min(vals),max(vals)
    pb,p=half_floor(lo)
    if hi-p<1e-9: db,d=0,0.0
    else: db,d=half_ceil((hi-p)/255.0)
    def enc(v):
        if d==0: return 0, p
        k=int(round((v-p)/d)); k=max(0,min(255,k)); return k,p+d*k
    return (pb,db),enc
def fit_channel(v, loop, tol):
    n=len(v)
    if max(v)-min(v)<tol: return ('const',float(np.mean(v)))
    def slope(f):
        if loop: return (v[(f+1)%n]-v[(f-1)%n])/2
        if f==0: return v[1]-v[0]
        if f==n-1: return v[-1]-v[-2]
        return (v[f+1]-v[f-1])/2
    keys=sorted(set(list(range(0,n-1,200))+[n-1]))   # keys at most 255 frames apart (a u8 step)
    while True:
        k=sorted(keys); vals=[];tin=[];tout=[]
        rec=build(v,k,slope)
        err=np.abs(np.array([sample(rec,f) for f in range(n)])-v)
        if err.max()<tol or len(k)>=n: return rec
        keys.append(int(np.argmax(err)))
def build(v,keys,slope):
    keys=sorted(set(keys)); vals=[v[f] for f in keys]
    tins=[];touts=[]
    for i,f in enumerate(keys):
        s=slope(f)
        Lin=f-keys[i-1] if i>0 else 1; Lout=keys[i+1]-f if i+1<len(keys) else 1
        tins.append(s*Lin); touts.append(s*Lout)
    (pb,db),ev=qrange(vals); (t0b,dt0b),et0=qrange(tins); (t1b,dt1b),et1=qrange(touts)
    out=[];prev=0
    for i,f in enumerate(keys):
        cp,dv=ev(vals[i]); c0,dti=et0(tins[i]); c1,dto=et1(touts[i])
        out.append((f-prev,cp,c0,c1,f,dv,dti,dto)); prev=f
    return ('curve',(pb,db,t0b,dt0b,t1b,dt1b),out)
def sample(rec,t):
    if rec[0]=='const': return rec[1]
    ks=rec[2]
    i=0
    while i+1<len(ks) and ks[i+1][4]<=t: i+=1
    if i+1==len(ks): return ks[-1][5]
    a,b=ks[i],ks[i+1]
    return hermite(a[5],a[7],b[5],b[6],(t-a[4])/(b[4]-a[4]))
def encode_block(rec):
    hdr,ks=rec[1],rec[2]
    return struct.pack('<6H',*hdr)+b''.join(struct.pack('<4B',k[0],k[1],k[2],k[3]) for k in ks)
def write_mot(frames, loop, records):
    """records: list of (bone, component, flags, rec) where rec is ('const',v) | ('curve',...) | ('raw',channel,count,blockbytes)."""
    n=len(records)+1
    table=b''; blobs=b''; base=8+12*n
    for bone,comp,flags,rec in records:
        if rec[0]=='const':
            table+=struct.pack('<BBHIf',bone,comp,0,flags,rec[1]); continue
        if rec[0]=='raw':
            blk=rec[3]; count=rec[2]
        else:
            blk=encode_block(rec); count=len(rec[2])
        table+=struct.pack('<BBHII',bone,0x10|comp,count,flags,base+len(blobs)); blobs+=blk
    table+=struct.pack('<BBHII',127,255,65535,0,0)
    return b'mtb3'+struct.pack('<HBB',frames,n,loop)+table+blobs
# --- build ---
def upright(seq):
    """The hips' mean tilt moved into the waist and their mean heading dropped: the legs (the bunny girl's idle,
    placed by the game's leg IK) stand under her facing forward, the upper body leans as the source's did."""
    L0 = [Rot.from_euler('xyz', s[0][0]) for s in seq]
    M = Rot.from_quat(np.mean([q.as_quat() * (1 if q.as_quat()[3] >= 0 else -1) for q in L0], axis=0))
    # The mean heading is dropped (she faces forward; the port's Turn sets where), its tilt kept in the waist.
    f = M.apply([0.0, 0.0, 1.0])
    unturn = Rot.from_euler('y', -np.arctan2(f[0], f[2]))
    prev = None
    for s, l0 in zip(seq, L0):
        n0 = M.inv() * l0
        n1 = n0.inv() * unturn * l0 * Rot.from_euler('xyz', s[0][1])
        s[0][0] = n0.as_euler('xyz')
        e1 = n1.as_euler('xyz')
        if prev is not None:
            e1 = prev + (e1 - prev + np.pi) % (2 * np.pi) - np.pi
        s[0][1] = e1
        prev = e1

def build_motion(seq, ema5, MAP, tol=0.004, pelvis=1.0, head_forward=False):
    fc=len(seq)
    ents=dat_read(open(ema5,'rb').read()); idle=ents[19][1]; M=Mot(idle)
    ANIM=set(MAP)-{0}
    recs=[]
    for k,r in enumerate(M.records):
        if r.sentinel or r.bone in ANIM or r.bone==0: continue
        if r.constant is not None: recs.append((r.bone,r.component,r.flags,('const',r.constant))); continue
        if (r.bone in (20,24) and r.component in (0,2)) or r.bone==255:
            v=0.0 if r.bone==255 else float(np.mean([r.sample(f) for f in range(M.frame_count)]))
            recs.append((r.bone,r.component,r.flags,('const',v))); continue
        # other keyed leg curves (thighs, feet y): the idle's, resampled over the new length
        vals=np.array([r.sample(f*M.frame_count/fc) for f in range(fc)])
        recs.append((r.bone,r.component,r.flags,fit_channel(vals,True,tol)))
    base=np.array([0.0,1.117,0.0])
    hp=np.array([s[1] for s in seq])
    hp=(hp-hp.mean(0))*pelvis+hp.mean(0)+base   # --pelvis: how much of the hips' travel
    for c in range(3): recs.append((0,c,0,fit_channel(hp[:,c],True,0.002)))
    r0=np.array([s[0][0] for s in seq])
    for c in range(3): recs.append((0,3+c,0,fit_channel(r0[:,c],True,tol)))
    for b in sorted(ANIM):
        arr=np.array([s[0][b] for s in seq])
        if b==17: arr=(arr-arr.mean(0))*pelvis   # the pelvis: only its wiggle (the bend is the waist's), times --pelvis
        if head_forward and b in (3,4): arr[:,1]-=arr[:,1].mean()   # --head-forward: the neck and head's mean turn taken out
        for c in range(3): recs.append((b,3+c,0,fit_channel(arr[:,c],True,tol)))
    recs.sort(key=lambda r:(r[0] if r[0]!=255 else -1,r[1]))
    return write_mot(fc,1,recs), seq

if __name__ == '__main__':
    args = sys.argv[1:]
    opt = lambda k, d: (args.pop(args.index(k) + 1), args.remove(k))[0] if k in args else d
    slow = float(opt('--slow', 1.5)); sigma = float(opt('--smooth', 2.0)); fr = opt('--frames', None)
    tol = float(opt('--tolerance', 0.004)); pelvis = float(opt('--pelvis', 1.0))
    head_forward = '--head-forward' in args
    if head_forward: args.remove('--head-forward')
    src, rig, ema5, out = args[:4]
    names, rest, anim, fps = dump(os.path.abspath(src), tuple(int(x) for x in fr.split(':')) if fr else None)
    if fps != 30:
        print('note: the source runs at %d frames/s; the game at 30 (--slow %.3f keeps its speed)' % (fps, 30.0 / fps))
    bones = game_skeleton(ema5)
    if 'basis' in RIGS[rig]:
        # The armature's axes to the game's (Y up, +Z forward, +X her left): bone matrices M -> G M.
        G = np.eye(4); G[:3, :3] = np.array(RIGS[rig]['basis'], float)
        rest = np.einsum('ij,bjk->bik', G, rest); anim = np.einsum('ij,fbjk->fbik', G, anim)
    seq = retarget(names, rest, anim, RIGS[rig], bones, slow, sigma)
    upright(seq)
    mot, _ = build_motion(seq, ema5, RIGS[rig]['bones'], tol, pelvis, head_forward)
    if len(mot) > 0x3000:
        print('warning: %d bytes; the port takes up to 16384 with its own adjustments on top: try a larger --tolerance or less --slow' % len(mot))
    Mot(mot)
    open(out, 'wb').write(mot)
    print('%s: %d frames, %d bytes' % (out, len(seq), len(mot)))
