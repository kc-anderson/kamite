"""The NPC rail idle (src/kamite/npc_poses.cpp): retargets Quaternius' Universal Animation Library 2 (CC0)
Idle_Rail_Loop and Idle_Rail_Call to the game's 30-bone woman's skeleton and writes them as MOT into
src/kamite/npc_motions.inc.

  python -I tools/models/npc_rail.py <UAL2_Standard.glb> <extracted em/ema5.dat> <src/kamite/npc_motions.inc>

Upper body (bones 1-16, 26, 29): each bone's world rotation relative to the UAL T-pose (A_TPose), carried over to the
game's T-pose (its bones' rest rotations are all zero), hips held level; local Euler angles in the game's order
(R = Rz Ry Rx). Legs, hips and feet: ema5.dat motion 19 (the game's leg IK places the feet); the feet's targets and the
root are made constant, and a constant hips turn (bone 0, y) is added, so the port can move and turn the pose
(npc_poses.cpp). Curves: cubic Hermite keys added where the error is largest until it is under 0.003 rad.
Needs numpy and scipy.
"""
import json, math, os, struct, sys
import numpy as np
from scipy.spatial.transform import Rotation as Rot
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gamemodel import dat_read, scr_read, mdb_read, strip_tris
from mot import Mot, half, hermite

GLB, EMA5, OUT = sys.argv[1:4] if __name__ == '__main__' and len(sys.argv) >= 4 else (None, None, None)
UPPER=[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,26,29]

# --- glTF ---
class GLB_:
    def __init__(s,path):
        b=open(path,'rb').read(); l=struct.unpack_from('<I',b,12)[0]
        s.j=json.loads(b[20:20+l]); o=20+l; bl=struct.unpack_from('<I',b,o)[0]; s.bin=b[o+8:o+8+bl]
        s.nodes=s.j['nodes']; s.parent={}
        for i,n in enumerate(s.nodes):
            for c in n.get('children',[]): s.parent[c]=i
        s.joints=s.j['skins'][0]['joints']; s.name={i:s.nodes[i].get('name') for i in range(len(s.nodes))}
        s.byname={v:k for k,v in s.name.items()}
    def acc(s,i):
        a=s.j['accessors'][i]; bv=s.j['bufferViews'][a['bufferView']]
        n={'SCALAR':1,'VEC3':3,'VEC4':4,'MAT4':16}[a['type']]
        assert a['componentType']==5126
        off=bv.get('byteOffset',0)+a.get('byteOffset',0)
        return np.frombuffer(s.bin,'<f4',a['count']*n,off).reshape(a['count'],n) if n>1 else np.frombuffer(s.bin,'<f4',a['count'],off)
    def rest(s):
        T={};R={}
        for i,n in enumerate(s.nodes):
            T[i]=np.array(n.get('translation',[0,0,0]),float); R[i]=np.array(n.get('rotation',[0,0,0,1]),float)
        return T,R
    def anim(s,name):
        a=[x for x in s.j['animations'] if x['name']==name][0]
        T,R=s.rest(); tracks={}
        times=None
        for ch in a['channels']:
            smp=a['samplers'][ch['sampler']]; t=s.acc(smp['input']); v=s.acc(smp['output'])
            tracks[(ch['target']['node'],ch['target']['path'])]=(t,v); times=t if times is None or len(t)>len(times) else times
        return tracks,times
    def pose_at(s,tracks,t):
        T,R=s.rest(); T=dict(T); R=dict(R)
        for (node,path),(tt,vv) in tracks.items():
            k=np.searchsorted(tt,t)
            if k<=0: v=vv[0]
            elif k>=len(tt): v=vv[-1]
            else:
                f=(t-tt[k-1])/(tt[k]-tt[k-1])
                if path=='rotation':
                    a,b=vv[k-1],vv[k]
                    if np.dot(a,b)<0: b=-b
                    v=a*(1-f)+b*f; v=v/np.linalg.norm(v)
                else: v=vv[k-1]*(1-f)+vv[k]*f
            if path=='rotation': R[node]=v
            elif path=='translation': T[node]=v
        return T,R
    def world(s,T,R):
        WR={};WP={}
        def go(i):
            if i in WR: return
            p=s.parent.get(i)
            r=Rot.from_quat(R[i]).as_matrix()
            if p is None: WR[i]=r; WP[i]=T[i]
            else:
                go(p); WR[i]=WR[p]@r; WP[i]=WP[p]+WR[p]@T[i]
        for i in range(len(s.nodes)): go(i)
        return WR,WP
# --- the game skeleton ---
def model(d):
    kind,nodes=scr_read(d)
    bones=mdb_read(d,nodes[0]['mdb_off'])['bones']
    meshes=[]
    for nd in nodes:
        m=mdb_read(d,nd['mdb_off'])
        for vb in m['vbs']:
            if not vb['wt']: continue
            meshes.append((np.array(vb['pos']), vb['wt'], strip_tris(vb['flags'])))
    return bones, meshes


# --- retarget ---
MAP={1:'spine_01',2:'spine_03',3:'neck_01',4:'Head',
     5:'clavicle_r',6:'upperarm_r',7:'upperarm_r',8:'lowerarm_r',9:'hand_r',10:'middle_01_r',
     11:'clavicle_l',12:'upperarm_l',13:'upperarm_l',14:'lowerarm_l',15:'hand_l',16:'middle_01_l',
     26:'Head',29:'spine_03'}
PARENT=None
def game_parents():
    b=open(EMA5,'rb').read()
    bones,meshes=model(dat_read(b)[15][1]); return bones,meshes
def retarget(clip, nframes, hip_pitch_to_waist=True):
    g=GLB_(GLB); bones,_=game_parents()
    trT,_=g.anim('A_TPose'); WRt,_=g.world(*g.pose_at(trT,0))
    tr,times=g.anim(clip); dur=times[-1]
    out=[]   # per frame: {bone: (rx,ry,rz)}
    for f in range(nframes):
        t=dur*f/nframes   # looping: frame n == frame 0
        WR,WP=g.world(*g.pose_at(tr,t))
        W={}
        for gb in range(len(bones)):
            if gb in MAP:
                s=g.byname[MAP[gb]]; W[gb]=WR[s]@WRt[s].T
            elif gb==0: W[gb]=np.eye(3)   # hips: the idle's (no rotation)
        loc={}
        for gb,Wg in W.items():
            if gb==0: continue
            p=bones[gb]['parent']; Wp=W.get(p,np.eye(3)) if p>=0 else np.eye(3)
            L=Wp.T@Wg
            loc[gb]=Rot.from_matrix(L).as_euler('xyz')   # extrinsic x,y,z: L = Rz Ry Rx
        out.append(loc)
    # unwrap angles over time
    for gb in out[0]:
        arr=np.unwrap(np.array([o[gb] for o in out]),axis=0)
        for f,o in enumerate(out): o[gb]=arr[f]
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
def mot_len(d):
    fc,rc,_=struct.unpack_from('<HBB',d,4); end=8+12*rc
    for k in range(rc):
        b,ch,c,fl,pl=struct.unpack_from('<BBHII',d,8+12*k)
        if ch!=0xFF and ch&0xF0==0x10: end=max(end,pl+12+4*c)
    return end
def fnv(b):
    h=0x811C9DC5
    for x in b: h=((h^x)*0x01000193)&0xFFFFFFFF
    return h
def build_motion(clip, idle, tol=0.003, loop=1):
    fc,rc,_=struct.unpack_from('<HBB',idle,4); M=Mot(idle)
    seq=retarget(clip,fc); recs=[]
    for r in M.records:
        if r.sentinel or r.bone in UPPER: continue
        comp=r.component
        if r.constant is not None:
            recs.append((r.bone,comp,r.flags,('const',r.constant))); continue
        if (r.bone in (20,24) and comp in (0,2)) or r.bone==255:
            v=0.0 if r.bone==255 else float(np.mean([r.sample(f) for f in range(fc)]))
            recs.append((r.bone,comp,r.flags,('const',v))); continue
        k=[x for x in struct.unpack_from('<BBHII',idle,8+12*M.records.index(r))]
        recs.append((r.bone,comp,r.flags,('raw',k[1],k[2],idle[k[4]:k[4]+12+4*k[2]])))
    for b in UPPER:
        arr=np.array([s[b] for s in seq])
        for c in range(3): recs.append((b,3+c,0,fit_channel(arr[:,c],bool(loop),tol)))
    recs.append((0,4,0,('const',0.0)))   # the hips' turn: the port's NPC poses set it
    recs.sort(key=lambda r:(r[0] if r[0]!=255 else -1,r[1]))
    return write_mot(fc,loop,recs)
def main():
    ents=dat_read(open(EMA5,'rb').read())
    idles=[(i,ents[i][1][:mot_len(ents[i][1])]) for i in (19,29)]
    lean=build_motion('Idle_Rail_Loop',ents[19][1])
    call=build_motion('Idle_Rail_Call',ents[19][1],loop=0)
    Mot(lean); Mot(call)
    def arr(name,b):
        s='    inline constexpr uint8_t %s[%d] = {\n'%(name,len(b))
        for i in range(0,len(b),24): s+='        '+', '.join('0x%02X'%x for x in b[i:i+24])+',\n'
        return s+'    };\n'
    out='''// Generated by tools/models/npc_rail.py from Quaternius' Universal Animation Library 2 (CC0, Idle_Rail_Loop and
// Idle_Rail_Call, retargeted to the game's 30-bone woman's skeleton) and ema5.dat's idle (motion 19: legs, hips, feet).
// Do not edit by hand.
#pragma once
#include <cstdint>

namespace kamite::npc_poses::data
{
'''
    out+='    // The idles that can be swapped (FNV-1a of the motion bytes in the game data): ema5.dat motions 19 and 29.\n'
    out+='    inline constexpr uint32_t kIdleHashes[] = {%s};\n'%', '.join('0x%08Xu'%fnv(b) for i,b in idles)
    out+=arr('kRailLoop',lean)+arr('kRailCall',call)+'}\n'
    open(OUT,'w').write(out)
    print([ (i,len(b),hex(fnv(b))) for i,b in idles], len(lean), len(call))

if __name__ == '__main__':
    main()
