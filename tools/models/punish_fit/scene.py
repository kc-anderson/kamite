import numpy as np, math
from fk import *
from mesh import load_mesh, skin

def roty(a):
    c,s=math.cos(a),math.sin(a)
    return np.array([[c,0,s],[0,1,0],[-s,0,c]])  # x'=x c+z s, z'=-x s+z c (the game's)

class Actor:
    def __init__(self,path,scale=1.0):
        self.skel=skeleton(path); self.mesh=load_mesh(self.skel); self.scale=scale
        self.pos=np.zeros(3); self.yaw=0.0
    def world(self,v):  # model -> world
        return self.pos+ (roty(self.yaw)@(self.scale*np.asarray(v).T)).T

def align(gene,enemy,vec,mode,f12, fit=None):
    """func_001299F0 (+ an optional fit: dict forward/side/turn/height applied like punish_fit)."""
    fit=fit or {}
    turn=math.radians(fit.get('turn',0.0))
    if mode!=1:
        # Gene faces the enemy: from his current spot
        d=enemy.pos-gene.pos
        G0=math.atan2(d[0],d[2]) if np.linalg.norm(d[[0,2]])>1e-6 else gene.yaw
        enemy.yaw=G0+math.pi
        gene.yaw=G0+f12+turn
    else:
        enemy.yaw=(gene.yaw-turn)+math.pi
    off=roty(enemy.yaw)@np.array([vec[0]*enemy.scale,0,vec[1]*enemy.scale]) if not fit.get('unscaled') else roty(enemy.yaw)@np.array([vec[0],0,vec[1]])
    gene.pos=np.array([enemy.pos[0]+off[0],gene.pos[1],enemy.pos[2]+off[2]])
    facing=gene.yaw-turn
    sh=roty(facing)@np.array([fit.get('side',0.0)*enemy.scale,0,fit.get('forward',0.0)*enemy.scale])
    gene.pos=gene.pos+sh

class Track:
    """An actor playing motions back to back, moving by channel 255 (root motion)."""
    def __init__(self,actor): self.a=actor; self.seq=[]; 
    def start(self,motions):
        self.motions=[Motion(m) if isinstance(m,int) else m for m in motions]; self.i=0; self.t=0; self._anchor()
    def _anchor(self):
        self.p0=self.a.pos.copy(); m=self.motions[self.i]; self.c0=np.array([m.get(255,0,0),0,m.get(255,2,0)])
    def cur(self): return self.motions[self.i], self.t
    def advance(self):
        m=self.motions[self.i]
        self.t+=1
        if self.t>=m.frames-1 and self.i+1<len(self.motions):
            self.t=0; self.i+=1; self._anchor(); return
        self.t=min(self.t,m.frames-1)
        c=np.array([m.get(255,0,self.t),0,m.get(255,2,self.t)])
        self.a.pos=self.p0+roty(self.a.yaw)@(self.a.scale*(c-self.c0))
    def pose(self):
        m,t=self.cur(); return pose(self.a.skel,m,t)

def frame_world(actor, W, P):
    V=skin(actor.mesh,W,P)
    return actor.world(V), [actor.world(p) if p is not None else None for p in P]
