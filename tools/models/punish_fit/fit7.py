"""Fitter v7: v6's two steps with exact signed distances (libigl, fast winding number sign).
A: placement, heights, victim bend and size against the touches; Gene's torso pushed out of the victim.
B: per key, Gene's torso and arms (8 pre-rotation angles) against a signed-distance grid of the victim's mesh as A
   posed it: no Gene point deeper in the victim than the original pairing allows at that frame for that part + 1 cm
   (or a flat 1 cm where K asked for less than the original, e.g. Gorilla's folded arms), the arm touches kept, small
   angles and small changes from the previous key preferred. Seeds from a coarse grid per arm, then Powell."""
import numpy as np, math, igl, itertools
from scipy.ndimage import map_coordinates
from scipy.optimize import minimize
from scipy.spatial import cKDTree
import fit5
from fit5 import *
from punishes import PUNISH
import fit6
FWN=igl.SignedDistanceType.SIGNED_DISTANCE_TYPE_FAST_WINDING_NUMBER
POSE=list(range(6,14))
D=math.radians

def sdepth(p,V,T):
    S=igl.signed_distance(np.ascontiguousarray(p,dtype=np.float64),np.ascontiguousarray(V,dtype=np.float64),T,FWN)[0]
    return np.maximum(-S,0.0)

def clip_base(fr,gd,T,parts):
    T=np.ascontiguousarray(T,dtype=np.int64); out={}
    for f in fr:
        if not f['touch']: continue
        for part,ids in parts.items():
            out[(f['stage'],f['t'],part)]=float(sdepth(f['gV'][ids],f['vV'],T).max())
    return out
fit5.clip_depths=clip_base          # reference_data's base clipping, exact

def victim_full(P,f,O,sz):
    vd=P['vd']; dv={'v0':f['vV'],'wv':np.array([lift_w(int(b)) for b in vd])*P['scale'],'vbend':np.isin(vd,list(UPPER)).astype(float),
                    'vpiv':np.repeat(f['vP'][1][None],len(vd),0),'vax':np.repeat(f['vW1'][:,0][None],len(vd),0),'vpos':np.repeat(f['vpos'][None],len(vd),0)}
    return victim_xform(dv,np.repeat(np.asarray(O)[None],len(vd),0),sz)

def allowed_for(P,si,t,part):
    if P.get('flat_allow') is not None and part in P['flat_allow']: return 0.01
    return P['base'].get((si,t,part),0.0)+0.01

def add_collisions7(P, X, size):
    fr=P['fr']; new=[]; T64=np.ascontiguousarray(P['vT'],dtype=np.int64)
    for i,f in enumerate(fr):
        if not f['touch'] or f['t']%2: continue
        k0,k1,a=P['bracket'](f['stage'],f['t']); O=(X[k0]*(1-a)+X[k1]*a)
        Vm=victim_full(P,f,O,size); tree=cKDTree(Vm); nv=vnormals(Vm,P['vT'])
        for part,ids in P['parts'].items():
            d=gene_frame_data(fr,P['gd'],np.full(len(ids),i),ids)
            Gp=gene_xform(d,np.repeat(O[None],len(ids),0)); dep=sdepth(Gp,Vm,T64)
            allowed=allowed_for(P,f['stage'],f['t'],part); dist,j=tree.query(Gp)
            for q in np.nonzero(dep>allowed)[0]: new.append((i,int(ids[q]),int(j[q]),nv[j[q]],allowed))
    if P['C'] is None: P['C']=[]
    seen={(c[0],c[1]) for c in P['C']}
    for c in new:
        if (c[0],c[1]) not in seen: P['C'].append(c); seen.add((c[0],c[1]))
    return len(new)

def step_a(P, use_size=True, rounds=2):
    LOs=fit5.LO; HIs=fit5.HI
    fit5.LO=fit5.LO.copy(); fit5.HI=fit5.HI.copy(); fit5.LO[6:]=-1e-6; fit5.HI[6:]=1e-6
    if not use_size: pass
    x=solve(P,use_size=use_size)
    parts=P['parts']; P['parts']={'torso':parts['torso']}
    for r in range(rounds):
        X,sz=x[:P['n']*NP].reshape(P['n'],NP),1+x[-1]
        if add_collisions7(P,X,sz)==0: break
        x=solve(P,x0=x,use_size=use_size,wcoll=12.0)
    P['parts']=parts; P['C']=None
    fit5.LO=LOs; fit5.HI=HIs
    return x[:P['n']*NP].reshape(P['n'],NP),1+x[-1]

class Grid:
    def __init__(self,V,T,lo,hi,h=0.02):
        n=np.maximum(np.ceil((hi-lo)/h).astype(int)+1,2)
        ax=[lo[i]+h*np.arange(n[i]) for i in range(3)]
        G=np.stack(np.meshgrid(*ax,indexing='ij'),-1).reshape(-1,3)
        self.D=sdepth(G,V,T).reshape(n); self.lo=lo; self.h=h
    def __call__(self,p):
        return map_coordinates(self.D,((p-self.lo)/self.h).T,order=1,mode='constant',cval=0.0)

def step_b(P, X, sz, wt=1.0, wc=4.0, wr=0.15, ws=0.3, near=2):
    fr=P['fr']; ks=P['ks']; offs=P['offs']; T=P['T']; idx=P['idx']; T64=np.ascontiguousarray(P['vT'],dtype=np.int64)
    Xb=X.copy(); Xb[:,POSE]=0; lo=LO[POSE]; hi=HI[POSE]
    tf=[(fr[i]['stage'],fr[i]['t']) for i in T['I']]; arm=np.array([w>=1.0 for w in T['w']])
    hold={h[0]:h[3] for h in P['hold']}
    for si,fs in enumerate(ks):
        tw=PUNISH[P['name']][si].get('touch'); prev=np.zeros(len(POSE))
        for jj,fk in enumerate(fs):
            kk=offs[si]+jj
            if hold[kk] or tw is None: prev=np.zeros(len(POSE)); continue
            i=idx[(si,fk)]; f=fr[i]; O0=X[kk].copy()
            Vm=victim_full(P,f,O0,sz)
            cps={part:(ids,gene_frame_data(fr,P['gd'],np.full(len(ids),i),ids)) for part,ids in P['parts'].items()}
            allp=np.concatenate([gene_xform(dd,np.repeat(O0[None],len(ids),0)) for ids,dd in cps.values()])
            glo=np.maximum(allp.min(0)-0.4,Vm.min(0)-0.03); ghi=np.minimum(allp.max(0)+0.4,Vm.max(0)+0.03)
            grid=Grid(Vm,T64,glo,ghi) if np.all(ghi>glo) else None
            allow={part:allowed_for(P,si,fk,part) for part in cps}
            sel=[q for q,(s_,t_) in enumerate(tf) if s_==si and abs(t_-fk)<=near and arm[q]]
            if sel:
                d=gene_frame_data(fr,P['gd'],np.full(len(sel),i),T['G'][np.array(sel)])
                tgt=fit6.victim_targets(P,T,np.array(sel),O0,sz)
            def parts_cost(O):
                c=0.0
                if grid is None: return c
                for part,(ids,dd) in cps.items():
                    ex=np.maximum(grid(gene_xform(dd,np.repeat(O[None],len(ids),0)))-allow[part],0.0)
                    c+=wc*((ex.max()/0.01)**2+10*np.mean(ex**2)/1e-4)
                return c
            def cost(th):
                th=np.clip(th,lo,hi); O=O0.copy(); O[POSE]=th
                c=wr*np.sum((th/D(15))**2)+ws*np.sum(((th-prev)/D(10))**2)
                if sel:
                    G=gene_xform(d,np.repeat(O[None],len(sel),0)); c+=wt*np.mean(np.sum((G-tgt)**2,1))/0.0009
                return c+parts_cost(O)
            # seeds: per arm a coarse grid (raise, swing, elbow), the other arm and the torso at prev
            seeds=[prev.copy(),np.zeros(len(POSE))]
            vals=(-D(25),0.0,D(25))
            for arm_i in (2,5):            # l_raise.., r_raise.. offsets within POSE
                best=None
                for a,b,c_ in itertools.product(vals,vals,vals):
                    th=prev.copy(); th[arm_i:arm_i+3]=(a,b,c_); v=cost(th)
                    if best is None or v<best[0]: best=(v,th)
                seeds.append(best[1])
            s2=seeds[0].copy(); s2[2:5]=seeds[2][2:5]; s2[5:8]=seeds[3][5:8]; seeds.append(s2)
            res=[minimize(cost,np.clip(s,lo,hi),method='Powell',bounds=list(zip(lo,hi)),options=dict(maxiter=4000,xtol=1e-3,ftol=1e-5)) for s in seeds]
            r=min(res,key=lambda r:r.fun)
            Xb[kk,POSE]=np.clip(r.x,lo,hi); prev=Xb[kk,POSE].copy()
    return Xb

def frame_ctx(P,X,sz):
    """Per touch frame: its keys, step A's values there, the victim's signed-distance grid, Gene's part data."""
    fr=P['fr']; T64=np.ascontiguousarray(P['vT'],dtype=np.int64); ctx={}
    for i,f in enumerate(fr):
        if not f['touch']: continue
        k0,k1,a=P['bracket'](f['stage'],f['t']); O=X[k0]*(1-a)+X[k1]*a
        Vm=victim_full(P,f,O,sz)
        cps={part:(ids,gene_frame_data(fr,P['gd'],np.full(len(ids),i),ids)) for part,ids in P['parts'].items()}
        allp=np.concatenate([gene_xform(dd,np.repeat(O[None],len(ids),0)) for ids,dd in cps.values()])
        glo=np.maximum(allp.min(0)-0.4,Vm.min(0)-0.03); ghi=np.minimum(allp.max(0)+0.4,Vm.max(0)+0.03)
        ctx[i]=dict(k0=k0,k1=k1,a=a,O=O,grid=Grid(Vm,T64,glo,ghi) if np.all(ghi>glo) else None,cps=cps,
                    allow={part:allowed_for(P,f['stage'],f['t'],part) for part in cps})
    return ctx

def gene_xform_g(r, g, Og):
    """gene_xform with the rotations built once per frame (group) instead of per point: r per point (g0, wg, up,
    larm, lfore, rarm, rfore, gi = its group), g per group (pivots, matrices, gpos, gyaw), Og the values per group."""
    gi=r['gi']; rot=lambda X,piv,R: piv+np.einsum('nij,nj->ni',R,X-piv)
    Rt=mm(mm(g['W0'],pre_torso(Og)),tr(g['W0'])); p=r['g0'].copy()
    m=r['up']>0; p[m]=rot(p[m],g['P1'][gi[m]],Rt[gi[m]])
    for mask,fore,Psh,Pel,Wc,Wsh,psh,pel in ((r['larm'],r['lfore'],g['P13'],g['P14'],g['W11'],g['W13'],pre_lsh(Og),pre_lel(Og)),
                                             (r['rarm'],r['rfore'],g['P7'],g['P8'],g['W5'],g['W7'],pre_rsh(Og),pre_rel(Og))):
        Psh2=rot(Psh,g['P1'],Rt); Pel2=rot(Pel,g['P1'],Rt); Wc2=mm(Rt,Wc); Rs=mm(mm(Wc2,psh),tr(Wc2))
        m=mask>0; p[m]=rot(p[m],Psh2[gi[m]],Rs[gi[m]])
        Pel3=rot(Pel2,Psh2,Rs); Wsh3=mm(mm(Rs,Rt),Wsh); Re=mm(mm(Wsh3,pel),tr(Wsh3))
        m=fore>0; p[m]=rot(p[m],Pel3[gi[m]],Re[gi[m]])
    O=Og[gi]; p[:,1]+=O[:,3]*r['wg']
    c,s=np.cos(O[:,2]),np.sin(O[:,2]); gp=g['gpos'][gi]; q=p-gp
    G=np.stack([q[:,0]*c+q[:,2]*s,q[:,1],-q[:,0]*s+q[:,2]*c],1)+gp
    yaw=g['gyaw'][gi]; fc,fs=np.cos(yaw),np.sin(yaw)
    G[:,0]+=O[:,1]*fc+O[:,0]*fs; G[:,2]+=-O[:,1]*fs+O[:,0]*fc
    return G

def group_data(fr,frames):
    g={}
    for b in (1,7,8,13,14): g['P%d'%b]=np.array([fr[i]['gP'][b] for i in frames])
    for b in (0,5,7,11,13): g['W%d'%b]=np.array([fr[i]['gW'][b] for i in frames])
    g['gpos']=np.array([fr[i]['gpos'] for i in frames]); g['gyaw']=np.array([fr[i]['gyaw'] for i in frames])
    return g

def key_problem(P, X, Xb, kk, ctx, frames):
    """One key's cost, batched: Gene's part points over the frames it shapes (pose per frame = w * th + fixed),
    per-frame grids and row slices, per-row allowance; the arm touches over the same frames."""
    T=P['T']; fr=P['fr']; kp={}
    fl=[i for i in frames if ctx[i]['grid'] is not None]
    wf=lambda c:(1-c['a'])*(c['k0']==kk)+c['a']*(c['k1']==kk)
    ff=lambda c:(1-c['a'])*(Xb[c['k0'],POSE] if c['k0']!=kk else 0)+c['a']*(Xb[c['k1'],POSE] if c['k1']!=kk else 0)
    if fl:
        R={k:[] for k in ('g0','wg','up','larm','lfore','rarm','rfore','gi')}; allow=[]; starts=[]; grids=[]; n0=0
        for gidx,i in enumerate(fl):
            cx=ctx[i]; a0=n0
            for part,(ids,dd) in cx['cps'].items():
                for k in R:
                    R[k].append(np.full(len(ids),gidx) if k=='gi' else dd[k])
                allow.append(np.full(len(ids),cx['allow'][part])); starts.append(n0); n0+=len(ids)
            grids.append((cx['grid'],a0,n0))
        kp['R']={k:np.concatenate(v) for k,v in R.items()}; kp['allow']=np.concatenate(allow); kp['starts']=np.array(starts)
        kp['grids']=grids; kp['g']=group_data(fr,fl)
        kp['Og']=np.array([ctx[i]['O'] for i in fl]); kp['W']=np.array([wf(ctx[i]) for i in fl]); kp['F']=np.array([ff(ctx[i])*np.ones(len(POSE)) for i in fl])
    arm=np.array([w>=1.0 for w in T['w']]); fs=set(frames)
    sel=np.array([q for q in range(len(T['I'])) if arm[q] and T['I'][q] in fs],dtype=int)
    if len(sel):
        kp['ds']={k:v[sel] for k,v in T.items() if isinstance(v,np.ndarray) and len(v)==len(T['I'])}
        kp['Os']=np.array([X[k0]*(1-a)+X[k1]*a for k0,k1,a in zip(T['K0'][sel],T['K1'][sel],T['A'][sel])])
        cs=[ctx[T['I'][q]] for q in sel]
        kp['Ws']=np.array([wf(c) for c in cs]); kp['Fs']=np.array([ff(c)*np.ones(len(POSE)) for c in cs])
    return kp

def key_cost(kp, th, sz, nb, wt=1.0, wc=4.0, wr=0.15, ws=0.3):
    c=wr*np.sum((th/D(15))**2)+ws*sum(np.sum(((th-n)/D(10))**2) for n in nb)
    if 'R' in kp:
        Og=kp['Og'].copy(); Og[:,POSE]=kp['W'][:,None]*th+kp['F']
        G=gene_xform_g(kp['R'],kp['g'],Og); dep=np.empty(len(G))
        for g,a,b in kp['grids']: dep[a:b]=g(G[a:b])
        ex=np.maximum(dep-kp['allow'],0.0)
        msq=np.add.reduceat(ex**2,kp['starts'])/np.diff(np.append(kp['starts'],len(ex)))
        c+=wc*((ex.max()/0.01)**2+10*msq.mean()/1e-4)
    if 'ds' in kp:
        O=kp['Os'].copy(); O[:,POSE]=kp['Ws'][:,None]*th+kp['Fs']
        c+=wt*np.mean(np.sum((gene_xform(kp['ds'],O)-victim_xform(kp['ds'],O,sz))**2,1))/0.0009
    return c

def refine(P, X, Xb, sz, ctx, sweeps=3, grid_seeds=True):
    """Each free key against every frame it shapes (angles in between are interpolated), its neighbours held.
    First sweep: seeds from a coarse grid per arm (raise, swing, elbow)."""
    lo=LO[POSE]; hi=HI[POSE]; hold={h[0]:h[3] for h in P['hold']}
    stage_of={}
    for si,fs in enumerate(P['ks']):
        for jj in range(len(fs)): stage_of[P['offs'][si]+jj]=(si,jj,len(fs))
    free=[kk for kk in range(P['n']) if not hold[kk] and PUNISH[P['name']][stage_of[kk][0]].get('touch') is not None]
    vals=(-D(25),0.0,D(25))
    for sw in range(sweeps):
        for kk in free:
            si,jj,L=stage_of[kk]; o=P['offs'][si]
            frames=[i for i,c in ctx.items() if (c['k0']==kk and c['a']<1) or (c['k1']==kk and c['a']>0)]
            if not frames: continue
            kp=key_problem(P,X,Xb,kk,ctx,frames)
            nb=[Xb[o+j,POSE] for j in (jj-1,jj+1) if 0<=j<L and (sw>0 or j<jj)]
            f=lambda th: key_cost(kp,np.clip(th,lo,hi),sz,nb)
            cur=Xb[kk,POSE].copy(); seeds=[cur]
            if sw==0 and grid_seeds:
                comb=cur.copy()
                for arm_i in (2,5):
                    best=None
                    for a,b,c_ in itertools.product(vals,vals,vals):
                        th=cur.copy(); th[arm_i:arm_i+3]=(a,b,c_); v=f(th)
                        if best is None or v<best[0]: best=(v,th)
                    comb[arm_i:arm_i+3]=best[1][arm_i:arm_i+3]
                seeds.append(comb)
                if jj>0: seeds.append(Xb[kk-1,POSE].copy())
            res=[minimize(f,np.clip(s_,lo,hi),method='Powell',bounds=list(zip(lo,hi)),options=dict(maxiter=3000,xtol=1e-3,ftol=1e-5)) for s_ in seeds]
            r=min(res,key=lambda r:r.fun)
            if r.fun<f(cur): Xb[kk,POSE]=np.clip(r.x,lo,hi)
    return Xb

def parts_excess(P,X,sz,every=2):
    T64=np.ascontiguousarray(P['vT'],dtype=np.int64); res={p:[] for p in P['parts']}
    for i,f in enumerate(P['fr']):
        if not f['touch'] or f['t']%every: continue
        k0,k1,a=P['bracket'](f['stage'],f['t']); O=X[k0]*(1-a)+X[k1]*a
        Vm=victim_full(P,f,O,sz)
        for part,ids in P['parts'].items():
            d=gene_frame_data(P['fr'],P['gd'],np.full(len(ids),i),ids)
            dep=sdepth(gene_xform(d,np.repeat(O[None],len(ids),0)),Vm,T64).max()
            res[part].append(dep-P['base'].get((f['stage'],f['t'],part),0))
    return {p:(float(np.mean(np.maximum(v,0))),float(np.max(v))) for p,v in res.items()}

def fit(name,touches,base,path,scale=1.0,use_size=True,flat_allow=None):
    P=prepare(name,touches,base,path,scale); P['flat_allow']=flat_allow
    X,sz=step_a(P,use_size)
    Xb=X.copy(); Xb[:,POSE]=0
    Xb=refine(P,X,Xb,sz,frame_ctx(P,X,sz))
    return P,Xb,sz
