import numpy as np, math
from scipy.optimize import least_squares
from scipy.sparse import lil_matrix
from fit import reference, map_points, stage_keys
from sim import *
from contacts import dominant
from punishes import PUNISH

UPPER=set(range(1,17))|{26,27,28,29,30}
def lift_w(b):
    if b in UPPER or b in (0,17): return 1.0
    if b in (18,22): return 0.7
    if b in (19,23): return 0.4
    return 0.0
NAMES=('forward','side','turn','gene_height','victim_height','victim_bend')
NP=6
SCALE=np.array([1,1,0.15,1,1,0.15])
LO=np.array([-0.5,-0.5,-0.35,-0.15,-0.15,-0.35]); HI=np.array([0.5,0.5,0.35,0.06,0.25,0.35])

def prepare(name, touches, path):
    fr=simulate(name,path,1.0,keep_mesh=True)
    g=actor(GENE_PATH); v=actor(path)
    gd=dominant(g.mesh); vd=dominant(v.mesh)
    vmap=map_points(touches,v.mesh)
    ks=stage_keys(name,fr); idx={(f['stage'],f['t']):i for i,f in enumerate(fr)}
    offs=[];n=0
    for fs in ks: offs.append(n); n+=len(fs)
    rows=[]
    for t,vj in zip(touches,vmap):
        if vj<0 or (t['stage'],t['t']) not in idx: continue
        f=fr[idx[(t['stage'],t['t'])]]; fs=np.array(ks[t['stage']])
        j=min(max(np.searchsorted(fs,t['t'],side='right')-1,0),len(fs)-2) if len(fs)>1 else 0
        a=0.0 if len(fs)==1 else (t['t']-fs[j])/(fs[j+1]-fs[j])
        b=int(vd[vj])
        rows.append(dict(k0=offs[t['stage']]+j,k1=offs[t['stage']]+min(j+1,len(fs)-1),a=a,g0=f['gV'][t['gi']],wg=lift_w(int(gd[t['gi']])),
                         gpos=f['gpos'],gyaw=f['gyaw'],v0=f['vV'][vj],wv=lift_w(b)*f['vscale'],bend=float(b in UPPER),piv=f['vP'][1],ax=f['vW1'][:,0],
                         stage=t['stage'],t=t['t'],gp=t['gp'],vp=t['vp']))
    hold=[]  # (key, gene heights forced to 0, victim height and bend forced to 0)
    SUPPORT=(0,2,17,19,23)
    last_stage=len(ks)-1
    for si,fs in enumerate(ks):
        tw=PUNISH[name][si].get('touch')
        for jj,fk in enumerate(fs):
            i=idx.get((si,fk)); f=fr[i]
            zg=f['gP'][list(SUPPORT)][:,1].min()<0.3; zv=f['vP'][list(SUPPORT)][:,1].min()<0.3
            if tw is None or fk>tw[1]+6: zg=zv=True
            if (si==last_stage and jj==len(fs)-1) or (si==0 and jj==0): zg=zv=True
            hold.append((offs[si]+jj,zg,zv))
    legs=[]
    for si,fs in enumerate(ks):
        for jj,fk in enumerate(fs):
            i=idx.get((si,fk))
            if i is None: continue
            f=fr[i]
            for side_,(lst,par) in enumerate(((f['vlegs'],4),(f['glegs'],3))):
                for hip,L,tgt in lst: legs.append((offs[si]+jj,par,hip,L,tgt))
    return dict(fr=fr,ks=ks,offs=offs,n=n,rows=rows,legs=legs,hold=hold,idx=idx,name=name)

def model(P, X):
    R=P['rows']
    K0=np.array([r['k0'] for r in R]); K1=np.array([r['k1'] for r in R]); A=np.array([r['a'] for r in R])[:,None]
    O=X[K0]*(1-A)+X[K1]*A
    g0=np.array([r['g0'] for r in R]); gpos=np.array([r['gpos'] for r in R]); gyaw=np.array([r['gyaw'] for r in R])
    wg=np.array([r['wg'] for r in R]); v0=np.array([r['v0'] for r in R]); wv=np.array([r['wv'] for r in R])
    bend=np.array([r['bend'] for r in R]); piv=np.array([r['piv'] for r in R]); ax=np.array([r['ax'] for r in R])
    p=g0.copy(); p[:,1]+=O[:,3]*wg
    c,s=np.cos(O[:,2]),np.sin(O[:,2]); q=p-gpos
    qx=q[:,0]*c+q[:,2]*s; qz=-q[:,0]*s+q[:,2]*c
    G=np.stack([qx,q[:,1],qz],1)+gpos
    fc,fs=np.cos(gyaw),np.sin(gyaw)
    G[:,0]+=O[:,1]*fc+O[:,0]*fs; G[:,2]+=-O[:,1]*fs+O[:,0]*fc
    th=O[:,5]*bend; k=ax/np.linalg.norm(ax,axis=1,keepdims=True); w=v0-piv
    ct,st=np.cos(th)[:,None],np.sin(th)[:,None]
    w2=w*ct+np.cross(k,w)*st+k*(np.einsum('ij,ij->i',k,w))[:,None]*(1-ct)
    V=piv+w2; V[:,1]+=O[:,4]*wv
    return G,V

def solve(P, use=(1,1,1,1,1,1), lam=(0.5,1.5), mu=4.0, ridge=(0.5,0.5,0.5,2.0,2.0,1.5)):
    n=P['n']; ks=P['ks']; offs=P['offs']; stages=PUNISH[P['name']]; use=np.array(use,float)
    free=np.nonzero(use)[0]
    def full(x):
        X=np.zeros((n,NP)); X[:,free]=x.reshape(n,len(free)); return X
    regs=[]  # (rowtype, keys)
    def residuals(x):
        X=full(x); G,V=model(P,X); r=[(G-V).ravel()]
        lg=[]
        for kk,par,hip,L,tgt in P['legs']:
            d=np.linalg.norm(hip+np.array([0,X[kk,par],0])-tgt)-L*0.995; lg.append(max(0.0,d)*8)
        r.append(np.array(lg))
        for si,fs in enumerate(ks):
            K=X[offs[si]:offs[si]+len(fs)]
            r.append((np.diff(K,axis=0)/SCALE*lam[0]*0.1).ravel())
            r.append((np.diff(K,n=2,axis=0)/SCALE*lam[1]*0.1).ravel())
            if 'loop' in stages[si]['label']: r.append(((K[0]-K[-1])/SCALE*mu*0.1))
            if si>0 and not stages[si].get('align'):
                r.append(((K[0]-X[offs[si-1]+len(ks[si-1])-1])/SCALE*mu*0.1))
            r.append((K/SCALE*np.array(ridge)*0.1).ravel())
        return np.concatenate(r)
    # sparsity
    x0=np.zeros(n*len(free)); m=len(residuals(x0))
    S=lil_matrix((m,len(x0)),dtype=int); row=0; nf=len(free)
    def mark(rows,keys):
        for kk in keys:
            for p in range(nf): S[rows,kk*nf+p]=1
    for r in P['rows']:
        for d in range(3): mark(row+d,[r['k0'],r['k1']])
        row+=3
    for kk,par,hip,L,tgt in P['legs']: mark(row,[kk]); row+=1
    for si,fs in enumerate(ks):
        o=offs[si]; L=len(fs)
        for j in range(L-1):
            for p in range(NP): mark(row,[o+j,o+j+1]); row+=1
        for j in range(L-2):
            for p in range(NP): mark(row,[o+j,o+j+1,o+j+2]); row+=1
        if 'loop' in stages[si]['label']:
            for p in range(NP): mark(row,[o,o+L-1]); row+=1
        if si>0 and not stages[si].get('align'):
            for p in range(NP): mark(row,[o,offs[si-1]+len(ks[si-1])-1]); row+=1
        for j in range(L):
            for p in range(NP): mark(row,[o+j]); row+=1
    assert row==m,(row,m)
    LOk=np.tile(LO,(n,1)); HIk=np.tile(HI,(n,1))
    for kk,zg,zv in P['hold']:
        if zg: LOk[kk,3]=-1e-6; HIk[kk,3]=1e-6
        if zv: LOk[kk,4]=LOk[kk,5]=-1e-6; HIk[kk,4]=HIk[kk,5]=1e-6
    lo=LOk[:,free].ravel(); hi=HIk[:,free].ravel()
    sol=least_squares(residuals,x0,jac_sparsity=S,bounds=(lo,hi),loss='soft_l1',f_scale=0.03,max_nfev=400,method='trf')
    X=full(sol.x)
    G0,V0=model(P,np.zeros((n,NP))); G1,V1=model(P,X)
    return dict(X=X,e0=np.linalg.norm(G0-V0,axis=1),e1=np.linalg.norm(G1-V1,axis=1),d1=G1-V1)

def offset_at(P,X,si,t):
    fs=np.array(P['ks'][si]); o=P['offs'][si]
    if len(fs)==1: return X[o]
    j=min(max(np.searchsorted(fs,t,side='right')-1,0),len(fs)-2); a=min(max((t-fs[j])/(fs[j+1]-fs[j]),0),1)
    return X[o+j]*(1-a)+X[o+j+1]*a

def reposed(P, X, path):
    """Frames with Gene / victim meshes re-posed with the fit (heights and bend through FK + leg IK, placement rigid)."""
    from fk import Motion, pose
    g=actor(GENE_PATH); v=actor(path)
    mots={}
    def M(i):
        if i not in mots: mots[i]=Motion(i)
        return mots[i]
    out=[]
    for f in P['fr']:
        o=offset_at(P,X,f['stage'],f['t']) if X is not None else np.zeros(NP)
        Wg,Pg=pose(g.skel,M(f['gm']),f['gt'],hip_dy=o[3]); Wv,Pv=pose(v.skel,M(f['vm']),f['vt'],hip_dy=o[4],bend=o[5])
        # world: Gene at its frame pos/yaw, victim likewise
        def world(a,pos,yaw,pts): return pos+(roty(yaw)@(a.scale*pts.T)).T
        gV=world(g,f['gpos'],f['gyaw'],skin(g.mesh,Wg,Pg)); vV=world(v,f['vpos'],f['vyaw'],skin(v.mesh,Wv,Pv))
        gV=apply_gene_rows(gV,f['gpos'],f['gyaw'],o)
        out.append(dict(f,gV=gV,vV=vV))
    return out

def apply_gene_rows(V,gpos,gyaw,o):
    c,s=math.cos(o[2]),math.sin(o[2]); q=V-gpos
    q=np.stack([q[:,0]*c+q[:,2]*s,q[:,1],-q[:,0]*s+q[:,2]*c],1)+gpos
    fc,fs=math.cos(gyaw),math.sin(gyaw)
    q[:,0]+=o[1]*fc+o[0]*fs; q[:,2]+=-o[1]*fs+o[0]*fc
    return q
