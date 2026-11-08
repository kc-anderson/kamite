import numpy as np, math
from scipy.spatial.transform import Rotation as Rot
from common import *

def rot(e):  # game order: R = Rz Ry Rx (extrinsic xyz)
    return Rot.from_euler('xyz', e).as_matrix()

class Motion:
    def __init__(self, idx):
        t,b=motion(idx); assert t=='MOT',(idx,t)
        self.idx=idx; self.m=Mot(b); self.frames=self.m.frame_count; self.loop=self.m.loop
        self.ch={}
        for r in self.m.records:
            if r.sentinel or r.component>5: continue
            self.ch[(r.bone,r.component)]=r
    def get(self,bone,comp,t,default=0.0):
        r=self.ch.get((bone,comp))
        return r.sample(t) if r else default
    def has(self,bone,comp): return (bone,comp) in self.ch

def pose(skel, mot, t, ik=True, hip_dy=0.0, bend=0.0):
    """World (model-space) matrices and positions per bone at motion time t. Model space: the object's own (before yaw/scale/pos)."""
    bones=skel['bones']; n=len(bones)
    W=[None]*n; P=[None]*n
    for i,b in enumerate(bones):
        e=[mot.get(i,3+c,t) for c in range(3)]
        if i==1: e[0]+=bend
        if i==0:
            tr=np.array([mot.get(0,c,t,b['t'][c]) for c in range(3)]); tr[1]+=hip_dy
        else:
            tr=np.array(b['t'],float)
        R=rot(e)
        p=b['parent']
        if p<0 or p>=n or p==i or W[p] is None:
            W[i]=R; P[i]=tr if i==0 else np.array(b['t'],float)
        else:
            W[i]=W[p]@R; P[i]=P[p]+W[p]@tr
    if ik:
        for th,kn,an in ((18,19,20),(22,23,24)):
            if an<n and mot.has(an,0):
                tgt=np.array([mot.get(an,c,t) for c in range(3)])
                legik(skel,W,P,th,kn,an,tgt,mot,t)
    return W,P

def legik(skel,W,P,th,kn,an,tgt,mot,t):
    bones=skel['bones']
    a=np.linalg.norm(bones[kn]['t']); b=np.linalg.norm(bones[an]['t'])
    hip=P[th]; d=tgt-hip; L=np.linalg.norm(d)
    L=min(max(L,1e-4),a+b-1e-5)
    dn=d/np.linalg.norm(d)
    # bend plane from the FK knee (thigh rotation) : pole = FK knee direction
    knee_fk=P[kn]; pole=knee_fk-hip; pole=pole-dn*np.dot(pole,dn)
    if np.linalg.norm(pole)<1e-6:
        pole=W[th]@np.array([0,0,1.0]); pole=pole-dn*np.dot(pole,dn)
    pole/=np.linalg.norm(pole)+1e-12
    # knee bends forward: put the knee on the pole side
    cosA=(a*a+L*L-b*b)/(2*a*L); cosA=max(-1,min(1,cosA)); sinA=math.sqrt(1-cosA*cosA)
    knee=hip+dn*a*cosA+pole*a*sinA
    ankle=hip+dn*L
    P[kn]=knee; P[an]=ankle
    # orientation for children/skinning: rebuild W for thigh and knee to point along new segments
    def aim(Wold,local_dir,new_dir):
        cur=Wold@ (local_dir/np.linalg.norm(local_dir))
        v=np.cross(cur,new_dir); s=np.linalg.norm(v); c=np.dot(cur,new_dir)
        if s<1e-8: return Wold
        k=v/s; ang=math.atan2(s,c)
        return Rot.from_rotvec(k*ang).as_matrix()@Wold
    W[th]=aim(W[th],np.array(bones[kn]['t']),(knee-hip)/np.linalg.norm(knee-hip))
    W[kn]=aim(W[th],np.array(bones[an]['t']),(ankle-knee)/np.linalg.norm(ankle-knee))
    # foot: keep its motion rotation as world? use parent chain
    e=[mot.get(an,3+c,t) for c in range(3)]
    W[an]=W[kn]@rot(e)
    for ch in range(len(bones)):
        if bones[ch]['parent']==an:
            P[ch]=P[an]+W[an]@np.array(bones[ch]['t']); W[ch]=W[an]@rot([mot.get(ch,3+c,t) for c in range(3)])
