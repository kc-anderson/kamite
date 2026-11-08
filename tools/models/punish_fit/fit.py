import numpy as np, math, json, sys
from scipy.optimize import least_squares
from scipy.spatial import cKDTree
from sim import *
from contacts import *
from punishes import PUNISH
from report1 import REF, MAIN_G, LIMB

KEYSTEP=4
PART_BONES={}
for b,p in VIC_PART.items(): PART_BONES.setdefault(p,set()).add(b)

def thresh(gp): return 0.05 if ('knee' in gp or 'foot' in gp or 'thigh' in gp) else 0.03

def reference(name):
    body,_=REF[name]; path=EX+'/em/%s.dat'%body
    fr=simulate(name,path,1.0,keep_mesh=True)
    g=actor(GENE_PATH); v=actor(path)
    gd=dominant(g.mesh); vd=dominant(v.mesh)
    rest=v.mesh['rest']; VR=v.mesh['V']
    touches=[]
    for k,f in enumerate(fr):
        if not f['touch']: continue
        for (gp,vp),(d,gi,vi) in frame_contacts(f,gd,vd,thr=0.05).items():
            if gp not in MAIN_G or d>thresh(gp): continue
            b=int(vd[vi])
            touches.append(dict(k=k,stage=f['stage'],t=f['t'],gp=gp,vp=vp,gi=int(gi),b=b,local=(VR[vi]-rest[b]).tolist(),d=float(d)))
    return fr,touches

def map_points(touches, mesh):
    vd=dominant(mesh); V=mesh['V']; rest=mesh['rest']; out=[]
    cache={}
    for t in touches:
        key=(t['vp'],t['b'],tuple(np.round(t['local'],4)))
        if key not in cache:
            cand=np.nonzero(np.isin(vd,list(PART_BONES[t['vp']])))[0]
            if len(cand)==0: cache[key]=-1
            else:
                b=min(t['b'],len(rest)-1)
                target=rest[b]+np.array(t['local'])
                cache[key]=int(cand[np.argmin(np.linalg.norm(V[cand]-target,axis=1))])
        out.append(cache[key])
    return out

def stage_keys(name, fr):
    """Key frames per stage: [(stage, [frames...])]."""
    ks=[]
    for si,stg in enumerate(PUNISH[name]):
        L=sum(1 for f in fr if f['stage']==si)
        fs=list(range(0,L,KEYSTEP)); 
        if fs[-1]!=L-1: fs.append(L-1)
        ks.append(fs)
    return ks

def interp(keys, vals, t):
    i=np.searchsorted(keys,t,side='right')-1; i=min(max(i,0),len(keys)-2)
    if len(keys)==1: return vals[0]
    a=(t-keys[i])/(keys[i+1]-keys[i]); a=min(max(a,0),1)
    return vals[i]*(1-a)+vals[i+1]*a

def apply_gene(P, gpos, gyaw, off):
    fwd,side,turn=off
    c,s=math.cos(turn),math.sin(turn)
    R=np.array([[c,0,s],[0,1,0],[-s,0,c]])
    q=(P-gpos)@R.T+gpos
    fc,fs=math.cos(gyaw),math.sin(gyaw)
    d=np.array([side*fc+fwd*fs,0,-side*fs+fwd*fc])
    return q+d

def solve(name, fr_ref, touches, path, dynamic=True, lam=(0.6,1.5), mu=4.0, ridge=0.05, w_turn=0.15):
    fr=simulate(name,path,1.0,keep_mesh=True)
    v=actor(path); vmap=map_points(touches,v.mesh)
    stages=PUNISH[name]; ks=stage_keys(name,fr)
    idx_by_stage_t={(f['stage'],f['t']):i for i,f in enumerate(fr)}
    nparam=3
    offs=[]; n=0
    for fs in ks: offs.append(n); n+=len(fs) if dynamic else 1
    T=[t for t,vj in zip(touches,vmap) if vj>=0]; VJ=[vj for vj in vmap if vj>=0]
    def unpack(x): return x.reshape(-1,3)
    def off_at(X,si,t):
        if not dynamic: return X[offs[si]]
        fs=ks[si]; return interp(np.array(fs),X[offs[si]:offs[si]+len(fs)],t)
    def residuals(x):
        X=unpack(x); r=[]
        for t,vj in zip(T,VJ):
            i=idx_by_stage_t.get((t['stage'],t['t']))
            if i is None: continue
            f=fr[i]; o=off_at(X,t['stage'],t['t'])
            g=apply_gene(f['gV'][t['gi']],f['gpos'],f['gyaw'],(o[0],o[1],o[2]))
            r.extend((g-f['vV'][vj])*(1.0))
        # regularization
        for si,fs in enumerate(ks):
            if dynamic:
                K=X[offs[si]:offs[si]+len(fs)]
                d1=np.diff(K,axis=0); r.extend((d1*np.array([1,1,w_turn])*lam[0]).ravel())
                d2=np.diff(K,n=2,axis=0); r.extend((d2*np.array([1,1,w_turn])*lam[1]).ravel())
                if 'loop' in stages[si]['label']: r.extend(((K[0]-K[-1])*mu).ravel())
                if si>0 and not stages[si].get('align'):
                    prev=X[offs[si-1]+len(ks[si-1])-1]; r.extend(((K[0]-prev)*mu).ravel())
            r.extend((X[offs[si]:offs[si]+(len(fs) if dynamic else 1)]*ridge).ravel())
        return np.array(r)
    x0=np.zeros(n*3)
    base=residuals(x0)
    sol=least_squares(residuals,x0,loss='soft_l1',f_scale=0.03,max_nfev=200)
    X=unpack(sol.x)
    # touch errors before/after
    def touch_err(X):
        e=[]
        for t,vj in zip(T,VJ):
            i=idx_by_stage_t.get((t['stage'],t['t']))
            if i is None: continue
            f=fr[i]; o=off_at(X,t['stage'],t['t']) if X is not None else np.zeros(3)
            g=apply_gene(f['gV'][t['gi']],f['gpos'],f['gyaw'],tuple(o))
            e.append(np.linalg.norm(g-f['vV'][vj]))
        return np.array(e)
    e0=touch_err(None); e1=touch_err(X)
    curves=[]
    for si,fs in enumerate(ks):
        K=X[offs[si]:offs[si]+(len(fs) if dynamic else 1)]
        curves.append(dict(stage=si,frames=fs if dynamic else [0],vals=K.tolist()))
    return dict(fr=fr,curves=curves,e0=e0,e1=e1,off_at=lambda si,t:off_at(X,si,t),T=T,VJ=VJ)
