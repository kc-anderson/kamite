"""Cobra Twist headlock at a new enemy size (K, Nov 11): Gene keeps his size; his hands stay together (as the
approved 1.0 fit's) and at the same spot on the victim's neck as in the 1.0 fit, scaled to the body; his pose steady.
Free per key: Gene's height (<= 0: knees bend, never stretch), a whole-body lift (0..8 cm: feet leave the ground
rather than the legs stretching), torso and arms. The victim's placement / height / bend from step A (feet planted),
which runs with the 1.0 fit's pose held. K's rule (Nov 11): Cobra is a vertical move, so the size difference shows and
is kept (no pair scaling to hide it, as Spanking does); re-pose instead. claude/cobra-clasp.md.

    GODHAND_EXTRACT=<extract> python -I cobra_hold.py <type> <group> <size> [lift max m, default 0]
    (writes fits7/rep_Cobra_<type>_<size> for the type's representative, else g_Cobra_<group>_<size>; emit7 then)"""
import sys,os,json,time,numpy as np
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
import fit7
from fit7 import Grid, allowed_for, victim_full
from fit5 import gene_frame_data, gene_xform, prepare, NP
from fit_pose import touches_for, path_of, REP, OUT
from scipy.optimize import least_squares
from scipy.sparse import lil_matrix
D=np.radians
PARTS={'R arm':(7,8,9,10),'L arm':(13,14,15,16),'torso':(1,2,3)}
COLS=[3,6,7,8,9,10,11,12,13,14]      # gene height, torso x z, L raise swing elbow, R raise swing elbow, lift (col 14)
NC=len(COLS)
def gx(d,O):
    G=gene_xform(d,O[:,:NP]); G[:,1]+=O[:,14]; return G
def neckspot(P,f,O,sz):
    vd=P['vd']; V=victim_full(P,f,O,sz); N=V[vd==(3 if (vd==3).any() else 4)].mean(0)   # em09 has no neck-dominant vertices: the head's
    k=f['vW1'][:,0]/np.linalg.norm(f['vW1'][:,0]); th=O[5]
    K=np.array([[0,-k[2],k[1]],[k[2],0,-k[0]],[-k[1],k[0],0]]); R=np.eye(3)+np.sin(th)*K+(1-np.cos(th))*(K@K)
    return V,N,R@f['vW1']
def reference(P1,X1,sz1):
    """per (stage, t): the 1.0 fit's hands (centroid in the victim's chest frame from the neck, per unit of size) and
    their distance apart."""
    gd=P1['gd']; n=len(gd); rh=gd==10; lh=gd==16; out={}
    for f in P1['fr']:
        if not f['touch']: continue
        si,t=f['stage'],f['t']; k0,k1,a=P1['bracket'](si,t); O=X1[k0]*(1-a)+X1[k1]*a
        i=P1['idx'][(si,t)]; d=gene_frame_data(P1['fr'],gd,np.full(n,i),np.arange(n)); G=gene_xform(d,np.repeat(O[None],n,0))
        V,N,W=neckspot(P1,f,O,sz1); H=(G[rh].mean(0)+G[lh].mean(0))/2
        out[(si,t)]=(W.T@(H-N)/(P1['scale']*sz1),np.linalg.norm(G[rh].mean(0)-G[lh].mean(0)))
    return out
def frames_for(P,A,sz,ref,every=2,thin=5,ramp=(14,18)):
    fr=P['fr']; gd=P['gd']; T64=np.ascontiguousarray(P['vT'],dtype=np.int64)
    sets=[np.nonzero(np.isin(gd,b))[0][::thin] for b in PARTS.values()]
    rh=np.nonzero(gd==10)[0][::4]; lh=np.nonzero(gd==16)[0][::4]
    ids=np.concatenate(sets+[rh,lh]); cut=np.cumsum([0]+[len(s) for s in sets]+[len(rh),len(lh)])
    out=[]
    for i,f in enumerate(fr):
        if not f['touch'] or f['t']%every: continue
        si,t=f['stage'],f['t']; k0,k1,a=P['bracket'](si,t); O=A[k0]*(1-a)+A[k1]*a
        Vm,N,W=neckspot(P,f,O,sz); off,dh=ref[(si,t)]
        target=N+W@off*(P['scale']*sz)
        d=gene_frame_data(fr,gd,np.full(len(ids),i),ids); p0=gx(d,np.repeat(O[None],len(ids),0))
        glo=np.maximum(p0.min(0)-0.5,Vm.min(0)-0.03); ghi=np.minimum(p0.max(0)+0.5,Vm.max(0)+0.03)
        w=1.0 if si>0 else float(np.clip((t-ramp[0])/(ramp[1]-ramp[0]),0,1))
        out.append(dict(si=si,t=t,k0=k0,k1=k1,a=a,O=O,d=d,cut=cut,grid=Grid(Vm,T64,glo,ghi),target=target,dh=dh,
                        allow=[allowed_for(P,si,t,p) for p in PARTS],w=w))
    return out
MAX_APART=0.10   # the original em07's hands stay 6-10 cm apart; a 1.0 fit's wider spacing (fat em18: up to 22 cm) isn't copied
# Clipping allowed beyond the original's per part (m) and its weight. K (Nov 11, the strongman): rather Gene's chest
# in the victim's upper back (the camera faces the front) than his arm through the neck: bend him at the hips.
SLACK={'R arm':0.01,'L arm':0.01,'torso':0.01}; WEIGHT={'R arm':3.0,'L arm':3.0,'torso':3.0}
ABSOLUTE=set()   # parts held to SLACK outright, not to the original's own depth + SLACK
TORSO=np.radians(15.0)   # Gene's torso turn limit (x = bend at the hips, z = lean)
LIFT_UNIT=0.04   # lift only where needed: this much lift costs one unit
LIFT=3e-6     # set by job(): 1.20 needs none (no runtime change); the runtime has no lift yet
def solve(P,A,frames,max_nfev=60,w_neck=1.0):
    hold={h[0]:h[3] for h in P['hold']}
    used=sorted({f['k0'] for f in frames}|{f['k1'] for f in frames})
    free=[k for k in used if not hold[k] and k!=P['offs'][0]]   # the grab's first key stays the 1.0 fit's (no pop at the start)
    pos={k:j for j,k in enumerate(free)}; nf=len(free); th0=A[free][:,COLS].copy()
    sc=np.array([0.02]+[D(6)]*8+[0.02])            # smoothness unit per col
    dev=np.array([0.05]+[D(25)]*8+[0.06])          # how far from the start is worth a unit
    def O_of(f,th):
        O=f['O'].copy(); r0=th[pos[f['k0']]] if f['k0'] in pos else A[f['k0'],COLS]
        r1=th[pos[f['k1']]] if f['k1'] in pos else A[f['k1'],COLS]; O[COLS]=r0*(1-f['a'])+r1*f['a']; return O
    def frame_res(f,th):
        p=gx(f['d'],np.repeat(O_of(f,th)[None],len(f['d']['g0']),0)); c=f['cut']; out=[]
        for j in range(len(PARTS)):
            q=p[c[j]:c[j+1]]; pn=list(PARTS)[j]; base=0.0 if pn in ABSOLUTE and f['w']>=1.0 else f['allow'][j]   # held out outright once the lock is on
            out.append(np.maximum(f['grid'](q)-(base+SLACK[pn]),0)/0.01/np.sqrt(len(q))*WEIGHT[pn])
        hr=p[c[3]:c[4]].mean(0); hl=p[c[4]:c[5]].mean(0)
        out.append([max(np.linalg.norm(hr-hl)-(min(f['dh'],MAX_APART)+0.02),0)/0.01*f['w']])
        out.append(((hr+hl)/2-f['target'])/0.03*f['w']*w_neck)
        return np.concatenate(out)
    offs=P['offs']; seqs=[[offs[si]+j for j in range(len(fs))] for si,fs in enumerate(P['ks'])]
    def tail(x):
        th=x.reshape(nf,NC); full=A[:,COLS].copy(); out=[]
        for k,j in pos.items(): full[k]=th[j]
        for s in seqs[:3]: out.append((np.diff(full[s],2,axis=0)/sc).ravel())
        for s in seqs[1:3]: out.append(np.diff(full[s][:,0]+full[s][:,-1])/0.03)   # steady height in the pulls
        out.append((full[seqs[1][0]]-full[seqs[1][-1]])/(sc/4))   # the loop repeats
        out.append((full[seqs[0][-1]]-full[seqs[1][0]])/sc)       # grab -> loop
        out.append((full[seqs[1][-1]]-full[seqs[2][0]])/sc)       # loop -> final pull
        out.append(((th-th0)/dev).ravel()); out.append(th[:,-1]/LIFT_UNIT)   # lift only where needed
        return np.concatenate(out)
    def res(x): th=x.reshape(nf,NC); return np.concatenate([frame_res(f,th) for f in frames]+[tail(x)])
    S=lil_matrix((len(res(th0.ravel())),nf*NC),dtype=int); row=0
    for f in frames:
        n_=len(frame_res(f,th0))
        for k in (f['k0'],f['k1']):
            if k in pos: S[row:row+n_,NC*pos[k]:NC*pos[k]+NC]=1
        row+=n_
    t0=tail(th0.ravel())
    for j in range(nf*NC):
        x=th0.ravel().copy(); x[j]+=0.01; S[row+np.nonzero(tail(x)!=t0)[0],j]=1
    lo=np.array([-0.15]+[-TORSO]*2+[-D(50)]*6+[0.0]); hi=np.array([0.0]+[TORSO]*2+[D(50)]*6+[LIFT])
    LO=np.tile(lo,nf); HI=np.tile(hi,nf)
    r=least_squares(res,np.clip(th0.ravel(),LO+1e-6,HI-1e-6),bounds=(LO,HI),loss='soft_l1',max_nfev=max_nfev,diff_step=1e-3,jac_sparsity=S,verbose=0)
    X=A.copy()
    for k,j in pos.items(): X[k,COLS]=r.x.reshape(nf,NC)[j]
    return X
def report(X,frames):
    ex={p:[] for p in PARTS}; dc=[]; dn=[]
    for f in frames:
        O=X[f['k0']]*(1-f['a'])+X[f['k1']]*f['a']; p=gx(f['d'],np.repeat(O[None],len(f['d']['g0']),0)); c=f['cut']
        for j,pn in enumerate(PARTS): ex[pn].append(np.maximum(f['grid'](p[c[j]:c[j+1]])-f['allow'][j],0).max())
        if f['w']>0.5:
            hr=p[c[3]:c[4]].mean(0); hl=p[c[4]:c[5]].mean(0); dc.append(np.linalg.norm(hr-hl)); dn.append(np.linalg.norm((hr+hl)/2-f['target']))
    return ', '.join('%s %.1f/%.1f'%(k,np.mean(v)*100,np.max(v)*100) for k,v in ex.items())+'; hands apart %.0f/%.0f, off the neck spot %.0f/%.0f cm'%(np.mean(dc)*100,np.max(dc)*100,np.mean(dn)*100,np.max(dn)*100)
def job(t,g,s,lift=0.0,lift_unit=0.04):
    global LIFT, LIFT_UNIT
    LIFT_UNIT=lift_unit
    import fit_pose
    LIFT=max(lift,3e-6); t0=time.time()
    tch,base=touches_for('Cobra',t); rep=g==REP[t]; path=path_of(g,t if rep else None)
    P1=prepare('Cobra',tch,base,path,1.0)
    r1=OUT+'/%s_1.00'%('rep_Cobra_%s'%t if rep else 'g_Cobra_%02d'%g)
    if os.path.exists(r1+'.npy'): X1=np.load(r1+'.npy'); sz1=json.load(open(r1+'.json'))['size']
    else: X1=np.zeros((P1['n'],NP)); sz1=1.0   # Cobra's own body (the tall em07): the game's pairing is its 1.0 pose
    ref=reference(P1,X1,sz1)
    P=prepare('Cobra',tch,base,path,s); P['flat_allow']=None; P['fixpose']=X1[:,6:]
    A,sz=fit7.step_a(P); A[:,6:]=P['fixpose']; A=np.concatenate([A,np.zeros((len(A),1))],1)
    frames=frames_for(P,A,sz,ref); before=report(A,frames)
    X=solve(P,A,frames); after=report(X,frames)
    if lift>0: np.save(OUT+'/lift_Cobra_%02d_%.2f.npy'%(g,s),X[:,14])
    tag=('rep_Cobra_%s_%.2f'%(t,s)) if rep else ('g_Cobra_%02d_%.2f'%(g,s))
    fit_pose.save(tag,P,X[:,:14],sz,dict(name='Cobra',type=t,group=g,scale=s,rep=rep,hold=True,
                  pose_from='cobra_hold.py: the 1.0 fit\'s headlock held at this size',report=after))
    return 'Cobra %s g%02d %.2f: start %s -> %s (%.0fs)'%(t,g,s,before,after,time.time()-t0)
if __name__=='__main__':
    a=sys.argv[1:]; print(job(a[0],int(a[1]),float(a[2]),float(a[3]) if len(a)>3 else 0.0))
