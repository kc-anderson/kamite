"""K (Nov 8): Cobra Twist on human Elvis (em03) - Gene's right arm hung down the chest, never near his left hand, where
the other fitted bodies show a headlock (the hands clasped round the neck). em03's own refine (fit_pose --fix) had
traded the clasp for less clipping in his bigger chest and beads; the fat type's pose (em18) clasps but sinks the right
forearm up to 14 cm into them.

The clasp, solved: from the type's representative pose on this body's placement (step A), Gene's torso (x, z) and both
arms (raise, swing, elbow) per key, over every other touch frame together (least squares, the Jacobian's sparsity
from which keys shape a frame): the hands' centroids within CLASP (the original pair's ~5-10 cm; ramped in over the
grab's frames 14-18, where the original's hands meet), each part (R arm, L arm, torso) no deeper in the victim than
the original pair at that frame + 1 cm, smooth between keys (second differences), the loop repeating, the stages
joining, near the representative's angles. Placement, heights, bend and size stay step A's.
em03 (Nov 8): R arm past the original 5.0 / 13.9 -> 1.7 / 6.0 cm (mean / max), hands 12 / 22 -> 8 / 9 cm.

    GODHAND_EXTRACT=<extract> python -I cobra_clasp.py <group> [<group> ...]   (writes fits7/g_Cobra_<group>_1.00)"""
import sys, os, json, time, numpy as np
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from scipy.optimize import least_squares
from scipy.sparse import lil_matrix
D=np.radians
PARTS={'R arm':(7,8,9,10),'L arm':(13,14,15,16),'torso':(1,2,3)}
COLS=[6,7,8,9,10,11,12,13]                 # torso x, z, L raise, swing, elbow, R raise, swing, elbow
LIM=(15,15,50,50,50,50,50,50)              # deg
DEV=(10,10,30,30,30,30,30,30)              # deg: how far from the representative's angles is worth a unit
CLASP=0.08
def frames_for(P,A,sz,every=2,thin=5,clasp_ramp=(14,18)):
    import fit7
    from fit7 import Grid, allowed_for, victim_full
    from fit5 import gene_frame_data, gene_xform
    fr=P['fr']; gd=P['gd']; T64=np.ascontiguousarray(P['vT'],dtype=np.int64)
    sets=[np.nonzero(np.isin(gd,b))[0][::thin] for b in PARTS.values()]
    rh=np.nonzero(gd==10)[0][::8]; lh=np.nonzero(gd==16)[0][::8]
    ids=np.concatenate(sets+[rh,lh]); cut=np.cumsum([0]+[len(s) for s in sets]+[len(rh),len(lh)])
    out=[]
    for i,f in enumerate(fr):
        if not f['touch'] or f['t']%every: continue
        si,t=f['stage'],f['t']; k0,k1,a=P['bracket'](si,t); O=A[k0]*(1-a)+A[k1]*a
        Vm=victim_full(P,f,O,sz); d=gene_frame_data(fr,gd,np.full(len(ids),i),ids)
        p0=gene_xform(d,np.repeat(O[None],len(ids),0))
        glo=np.maximum(p0.min(0)-0.5,Vm.min(0)-0.03); ghi=np.minimum(p0.max(0)+0.5,Vm.max(0)+0.03)
        w=1.0 if si>0 else float(np.clip((t-clasp_ramp[0])/(clasp_ramp[1]-clasp_ramp[0]),0,1))
        out.append(dict(si=si,t=t,k0=k0,k1=k1,a=a,O=O,d=d,cut=cut,grid=Grid(Vm,T64,glo,ghi),
                        allow=[allowed_for(P,si,t,p) for p in PARTS],wc=w))
    return out
def solve(P,A,frames,allow_extra=0.01,max_nfev=80):
    from fit5 import gene_xform
    nc=len(COLS); hold={h[0]:h[3] for h in P['hold']}
    used=sorted({f['k0'] for f in frames}|{f['k1'] for f in frames}); free=[k for k in used if not hold[k]]
    pos={k:j for j,k in enumerate(free)}; nf=len(free); thA=A[free][:,COLS].copy()
    def O_of(f,th):
        O=f['O'].copy()
        r0=th[pos[f['k0']]] if f['k0'] in pos else A[f['k0'],COLS]
        r1=th[pos[f['k1']]] if f['k1'] in pos else A[f['k1'],COLS]
        O[COLS]=r0*(1-f['a'])+r1*f['a']; return O
    def frame_res(f,th):
        p=gene_xform(f['d'],np.repeat(O_of(f,th)[None],len(f['d']['g0']),0)); c=f['cut']; out=[]
        for j in range(len(PARTS)):
            q=p[c[j]:c[j+1]]; out.append(np.maximum(f['grid'](q)-(f['allow'][j]+allow_extra),0)/0.01/np.sqrt(len(q))*3)
        out.append([max(np.linalg.norm(p[c[3]:c[4]].mean(0)-p[c[4]:c[5]].mean(0))-CLASP,0)/0.02*f['wc']])
        return np.concatenate(out)
    offs=P['offs']; seqs=[[offs[si]+j for j in range(len(fs))] for si,fs in enumerate(P['ks'])]; devs=D(np.array(DEV,float))
    def tail(x):
        th=x.reshape(nf,nc); full=A[:,COLS].copy(); out=[]
        for k,j in pos.items(): full[k]=th[j]
        for s in seqs[:3]: out.append((np.diff(full[s],2,axis=0)/D(6)).ravel())
        out.append((full[seqs[1][0]]-full[seqs[1][-1]])/D(1.5))   # the loop repeats
        out.append((full[seqs[0][-1]]-full[seqs[1][0]])/D(4))     # grab -> loop
        out.append((full[seqs[1][-1]]-full[seqs[2][0]])/D(4))     # loop -> final pull
        out.append(((th-thA)/devs).ravel())
        return np.concatenate(out)
    def res(x):
        th=x.reshape(nf,nc); return np.concatenate([frame_res(f,th) for f in frames]+[tail(x)])
    S=lil_matrix((len(res(thA.ravel())),nf*nc),dtype=int); row=0
    for f in frames:
        n_=len(frame_res(f,thA))
        for k in (f['k0'],f['k1']):
            if k in pos: S[row:row+n_,nc*pos[k]:nc*pos[k]+nc]=1
        row+=n_
    t0=tail(thA.ravel())
    for j in range(nf*nc):
        x=thA.ravel().copy(); x[j]+=0.01; S[row+np.nonzero(tail(x)!=t0)[0],j]=1
    L=D(np.array(LIM,float)); lo=np.tile(-L,nf); hi=np.tile(L,nf)
    r=least_squares(res,np.clip(thA.ravel(),lo+1e-6,hi-1e-6),bounds=(lo,hi),loss='soft_l1',max_nfev=max_nfev,diff_step=1e-3,jac_sparsity=S)
    X=A.copy()
    for k,j in pos.items(): X[k,COLS]=r.x.reshape(nf,nc)[j]
    return X
def report(X,frames):
    from fit5 import gene_xform
    ex={p:[] for p in PARTS}; dc=[]
    for f in frames:
        O=X[f['k0']]*(1-f['a'])+X[f['k1']]*f['a']; p=gene_xform(f['d'],np.repeat(O[None],len(f['d']['g0']),0)); c=f['cut']
        for j,pn in enumerate(PARTS): ex[pn].append(np.maximum(f['grid'](p[c[j]:c[j+1]])-f['allow'][j],0).max())
        if f['wc']>0.5: dc.append(np.linalg.norm(p[c[3]:c[4]].mean(0)-p[c[4]:c[5]].mean(0)))
    return ', '.join('%s %.1f / %.1f'%(k,np.mean(v)*100,np.max(v)*100) for k,v in ex.items())+'; hands %.1f / %.1f cm'%(np.mean(dc)*100,np.max(dc)*100)
def job(g,s=1.0):
    import fit7, fit_pose
    from fit5 import prepare
    t0=time.time(); t='fat'
    tch,base=fit_pose.touches_for('Cobra',t)
    P=prepare('Cobra',tch,base,fit_pose.path_of(g,t if g==fit_pose.REP[t] else None),s); P['flat_allow']=None
    rp=os.path.join(fit_pose.OUT,'rep_Cobra_%s_%.2f.npy'%(t,s)); P['fixpose']=np.load(rp)[:,6:]
    X,sz=fit7.step_a(P)                                   # placement etc. with the representative's pose held
    A=X.copy(); A[:,6:]=P['fixpose']
    frames=frames_for(P,A,sz); before=report(A,frames)
    C=solve(P,A,frames)
    m=fit_pose.save('g_Cobra_%02d_%.2f'%(g,s),P,C,sz,dict(name='Cobra',type=t,group=g,scale=s,rep=False,clasp=True,
                    pose_from='clasp (cobra_clasp.py: the hands together round the neck)'))
    return 'Cobra g%02d %.2f: representative pose %s -> clasp %s (%.0fs)'%(g,s,before,report(C,frames),time.time()-t0)
if __name__=='__main__':
    from multiprocessing import Pool
    with Pool(2) as p:
        for line in p.imap_unordered(job,[int(g) for g in sys.argv[1:]]): print(line,flush=True)
