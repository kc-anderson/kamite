"""Fitter v5 (Nov 8, K: "adjust poses per enemy body type ... adjusting Gene's pose to reduce clipping to acceptable
values (as much clipping as original animations allow themselves)").
Per key: Gene's placement (forward, side, turn), height, torso (pre-rotation of bone 1 in bone 0's frame: x, z), each
arm (upper arm pre-rotated in the clavicle's frame: z raise, y swing; forearm in the upper arm's frame: y bend), the
victim's height and waist bend; per punish: the victim's size (0.95..1.05). The pre-rotations are what the port puts
in Gene's motion (mot_edit preRotate), so offline and in game agree. Touches by Gene's arms count fully, by his body
at 0.35. Collision: Gene's arm and torso points against the victim's mesh (nearest vertex and its normal), allowed as
deep as the original pairing's clipping at that frame for that part + 1 cm; found points become push-out constraints,
solved again (rounds)."""
import numpy as np, math
from scipy.optimize import least_squares
from scipy.sparse import lil_matrix
from scipy.spatial import cKDTree
from fit import map_points, stage_keys, MAIN_G
from sim import *
from contacts import dominant, frame_contacts, GENE_PART, VIC_PART
from punishes import PUNISH
from quality import vnormals
from winding import winding, near_tris

UPPER=set(range(1,17))|{26,27,28,29,30,31}
LARM=(13,14,15,16); RARM=(7,8,9,10); LFORE=(14,15,16); RFORE=(8,9,10)
ARMPART={'R hand','L hand','R forearm','L forearm','R upper arm','L upper arm','R shoulder','L shoulder'}
def lift_w(b):
    if b in UPPER or b in (0,17): return 1.0
    if b in (18,22): return 0.7
    if b in (19,23): return 0.4
    return 0.0
NAMES=('forward','side','turn','gene_height','victim_height','victim_bend',
       'torso_x','torso_z','l_raise','l_swing','l_elbow','r_raise','r_swing','r_elbow')
NP=len(NAMES)
D=math.radians
SCALE=np.array([1,1,0.15,1,1,0.15]+[0.15]*8)
LO=np.array([-0.5,-0.5,-0.35,-0.15,-0.15,-0.35,-D(15),-D(15),-D(35),-D(35),-D(30),-D(35),-D(35),-D(30)])
HI=-LO.copy(); HI[3]=0.06; HI[4]=0.25

def rx(a):
    c,s=np.cos(a),np.sin(a); z=np.zeros_like(a); o=np.ones_like(a)
    return np.stack([np.stack([o,z,z],-1),np.stack([z,c,-s],-1),np.stack([z,s,c],-1)],-2)
def ry(a):
    c,s=np.cos(a),np.sin(a); z=np.zeros_like(a); o=np.ones_like(a)
    return np.stack([np.stack([c,z,s],-1),np.stack([z,o,z],-1),np.stack([-s,z,c],-1)],-2)
def rz(a):
    c,s=np.cos(a),np.sin(a); z=np.zeros_like(a); o=np.ones_like(a)
    return np.stack([np.stack([c,-s,z],-1),np.stack([s,c,z],-1),np.stack([z,z,o],-1)],-2)
def mm(a,b): return np.einsum('nij,njk->nik',a,b)
def mv(a,v): return np.einsum('nij,nj->ni',a,v)
def tr(a): return np.transpose(a,(0,2,1))
# pre-rotations as the runtime builds them (parent's frame): torso Rz(z) Rx(x); left upper arm Rz(raise) Ry(swing);
# right upper arm Rz(-raise) Ry(-swing); forearms Ry(+-bend)
def pre_torso(O): return mm(rz(O[:,7]),rx(O[:,6]))
def pre_lsh(O): return mm(rz(O[:,8]),ry(O[:,9]))
def pre_rsh(O): return mm(rz(-O[:,11]),ry(-O[:,12]))
def pre_lel(O): return ry(O[:,10])
def pre_rel(O): return ry(-O[:,13])

def rot_x(t):
    t=np.atleast_1d(np.asarray(t,float)); c,s_=np.cos(t),np.sin(t)
    R=np.zeros((len(t),3,3)); R[:,0,0]=1; R[:,1,1]=c; R[:,1,2]=-s_; R[:,2,1]=s_; R[:,2,2]=c
    return R
def gene_xform(d,O,twist=None):
    """Gene's adjusted points (d: per-point frame data). twist: the left forearm about its length (radians, per point or
    one value; the runtime's 13th value: half on bone 15, half on the hand 16, as post-rotations)."""
    p=d['g0'].copy()
    Rt=mm(mm(d['W0'],pre_torso(O)),tr(d['W0']))   # world rotation about P1
    up=d['up'][:,None]>0
    rot=lambda X,piv,R: piv+mv(R,X-piv)
    p=np.where(up,rot(p,d['P1'],Rt),p)
    for side,(mask,fore,Pc,Psh,Pel,Wc,Wsh,psh,pel) in (('L',(d['larm'],d['lfore'],d['P11'],d['P13'],d['P14'],d['W11'],d['W13'],pre_lsh(O),pre_lel(O))),
                                                    ('R',(d['rarm'],d['rfore'],d['P5'],d['P7'],d['P8'],d['W5'],d['W7'],pre_rsh(O),pre_rel(O)))):
        Psh2=rot(Psh,d['P1'],Rt); Pel2=rot(Pel,d['P1'],Rt)
        Wc2=mm(Rt,Wc)                      # the clavicle after the torso turn
        Rs=mm(mm(Wc2,psh),tr(Wc2))         # world rotation of the shoulder pre-rotation
        p=np.where(mask[:,None]>0,rot(p,Psh2,Rs),p)
        Pel3=rot(Pel2,Psh2,Rs); Wsh3=mm(mm(Rs,Rt),Wsh)
        Re=mm(mm(Wsh3,pel),tr(Wsh3))
        p=np.where(fore[:,None]>0,rot(p,Pel3,Re),p)
        if side=='L' and twist is not None:
            n=len(p); Rx=rot_x(np.broadcast_to(np.asarray(twist,float)*0.5,(n,)))
            chain=lambda P: rot(rot(rot(P,d['P1'],Rt),Psh2,Rs),Pel3,Re)
            Rall=mm(Re,mm(Rs,Rt))
            W15=mm(Rall,d['W15']); P15=chain(d['P15']); T15=mm(mm(W15,Rx),tr(W15))
            p=np.where((d['lb15']+d['lb16'])[:,None]>0,rot(p,P15,T15),p)
            W16=mm(T15,mm(Rall,d['W16'])); P16=rot(chain(d['P16']),P15,T15); T16=mm(mm(W16,Rx),tr(W16))
            p=np.where(d['lb16'][:,None]>0,rot(p,P16,T16),p)
    p[:,1]+=O[:,3]*d['wg']
    c,s=np.cos(O[:,2]),np.sin(O[:,2]); q=p-d['gpos']
    G=np.stack([q[:,0]*c+q[:,2]*s,q[:,1],-q[:,0]*s+q[:,2]*c],1)+d['gpos']
    fc,fs=np.cos(d['gyaw']),np.sin(d['gyaw'])
    G[:,0]+=O[:,1]*fc+O[:,0]*fs; G[:,2]+=-O[:,1]*fs+O[:,0]*fc
    return G

def victim_xform(d,O,size,vkey='v0'):
    k=d['vax']/np.linalg.norm(d['vax'],axis=1,keepdims=True)
    th=O[:,5]*d['vbend']
    K=np.zeros((len(k),3,3)); K[:,0,1]=-k[:,2];K[:,0,2]=k[:,1];K[:,1,0]=k[:,2];K[:,1,2]=-k[:,0];K[:,2,0]=-k[:,1];K[:,2,1]=k[:,0]
    R=np.eye(3)[None]+np.sin(th)[:,None,None]*K+(1-np.cos(th))[:,None,None]*(K@K)
    V=d['vpiv']+mv(R,d[vkey]-d['vpiv']); V[:,1]+=O[:,4]*d['wv']
    return d['vpos']+size*(V-d['vpos'])

def gene_frame_data(fr,gd,I,G):
    d={}
    d['g0']=np.array([fr[i]['gV'][g] for i,g in zip(I,G)])
    gb=gd[G]; d['wg']=np.array([lift_w(int(b)) for b in gb])
    d['up']=np.isin(gb,list(UPPER)).astype(float)
    d['larm']=np.isin(gb,LARM).astype(float); d['lfore']=np.isin(gb,LFORE).astype(float)
    d['rarm']=np.isin(gb,RARM).astype(float); d['rfore']=np.isin(gb,RFORE).astype(float)
    for b in (1,5,7,8,11,13,14,15,16): d['P%d'%b]=np.array([fr[i]['gP'][b] for i in I])
    for b in (0,5,7,11,13,15,16): d['W%d'%b]=np.array([fr[i]['gW'][b] for i in I])
    d['lb15']=(gb==15).astype(float); d['lb16']=(gb==16).astype(float)
    d['gpos']=np.array([fr[i]['gpos'] for i in I]); d['gyaw']=np.array([fr[i]['gyaw'] for i in I])
    d['vpos']=np.array([fr[i]['vpos'] for i in I])
    d['vpiv']=np.array([fr[i]['vP'][1] for i in I]); d['vax']=np.array([fr[i]['vW1'][:,0] for i in I])
    return d

def collision_points(gd):
    return {'L arm':np.nonzero(np.isin(gd,LARM))[0][::7],'R arm':np.nonzero(np.isin(gd,RARM))[0][::7],
            'torso':np.nonzero(np.isin(gd,[1,2,3]))[0][::9]}

def clip_depths(fr_list, gd, vd_mesh_T, parts):
    """Per frame index and Gene part: max depth of Gene's points inside the victim's mesh (nearest vertex, normal)."""
    out={}
    for i,f in enumerate(fr_list):
        if not f['touch']: continue
        tree=cKDTree(f['vV'])
        for part,ids in parts.items():
            p=f['gV'][ids]; dist,j=tree.query(p)
            Tn=near_tris(f['vV'],vd_mesh_T,p); ins=winding(p,f['vV'],Tn)>0.5 if len(Tn) else np.zeros(len(p),bool)
            out[(f['stage'],f['t'],part)]=float(dist[ins].max()) if ins.any() else 0.0
    return out

def reference_data(name, refpath):
    """The original pairing's touches (with Gene vertex ids) and its own clipping per frame and part."""
    fr=simulate(name,refpath,1.0,keep_mesh=True)
    g=actor(GENE_PATH); v=actor(refpath); gd=dominant(g.mesh); vd=dominant(v.mesh)
    rest=v.mesh['rest']; VR=v.mesh['V']; touches=[]
    for k,f in enumerate(fr):
        if not f['touch']: continue
        for (gp,vp),(dd,gi,vi) in frame_contacts(f,gd,vd,thr=0.05).items():
            if gp not in MAIN_G or dd>(0.05 if ('knee' in gp or 'foot' in gp or 'thigh' in gp) else 0.03): continue
            b=int(vd[vi]); touches.append(dict(k=k,stage=f['stage'],t=f['t'],gp=gp,vp=vp,gi=int(gi),b=b,local=(VR[vi]-rest[b]).tolist(),d=float(dd)))
    parts=collision_points(gd)
    base=clip_depths(fr,gd,v.mesh['T'],parts)
    return touches, base

def retarget_neck(touches):
    """Cobra Twist on women and fat men (K): Gene's arm touches on the chest go round the neck instead."""
    out=[]
    for t in touches:
        if t['gp'] in ARMPART and t['vp']=='chest':
            t=dict(t,vp='neck',b=3,local=[0.0,0.03,0.02])
        out.append(t)
    return out

def prepare(name, touches, base, path, scale=1.0):
    fr=simulate(name,path,scale,keep_mesh=True)
    g=actor(GENE_PATH); v=actor(path,scale)
    gd=dominant(g.mesh); vd=dominant(v.mesh)
    vmap=map_points(touches,v.mesh)
    ks=stage_keys(name,fr); idx={(f['stage'],f['t']):i for i,f in enumerate(fr)}
    offs=[];n=0
    for fs in ks: offs.append(n); n+=len(fs)
    def bracket(si,t):
        fs=np.array(ks[si]); j=min(max(np.searchsorted(fs,t,side='right')-1,0),len(fs)-2) if len(fs)>1 else 0
        a=0.0 if len(fs)==1 else (t-fs[j])/(fs[j+1]-fs[j]); return offs[si]+j,offs[si]+min(j+1,len(fs)-1),a
    items=[(idx[(t['stage'],t['t'])],t['gi'],vj,1.0 if t['gp'] in ARMPART else 0.35) for t,vj in zip(touches,vmap) if vj>=0 and (t['stage'],t['t']) in idx]
    if len(items)>600:
        step=len(items)/600.0; items=[items[int(i*step)] for i in range(600)]
    I=np.array([it[0] for it in items]); G=np.array([it[1] for it in items]); VJ=np.array([it[2] for it in items])
    T=gene_frame_data(fr,gd,I,G); T['w']=np.array([it[3] for it in items]); T['G']=G; T['I']=I
    T['v0']=np.array([fr[i]['vV'][vj] for i,vj in zip(I,VJ)]); vb=vd[VJ]
    T['wv']=np.array([lift_w(int(b)) for b in vb])*scale; T['vbend']=np.isin(vb,list(UPPER)).astype(float)
    br=[bracket(fr[i]['stage'],fr[i]['t']) for i in I]; T['K0']=np.array([b[0] for b in br]); T['K1']=np.array([b[1] for b in br]); T['A']=np.array([b[2] for b in br])
    hold=[]; SUPPORT=(0,2,17,19,23); last=len(ks)-1
    for si,fs in enumerate(ks):
        tw=PUNISH[name][si].get('touch')
        for jj,fk in enumerate(fs):
            f=fr[idx[(si,fk)]]
            zg=f['gP'][list(SUPPORT)][:,1].min()<0.3; zv=f['vP'][list(SUPPORT)][:,1].min()<0.3
            late=tw is None or fk>tw[1]+6; ends=(si==last and jj==len(fs)-1) or (si==0 and jj==0)
            hold.append((offs[si]+jj,zg or late or ends,zv or late or ends,late or (si==last and jj==len(fs)-1)))
    legs=[]
    for si,fs in enumerate(ks):
        for jj,fk in enumerate(fs):
            f=fr[idx[(si,fk)]]
            for lst,par in ((f['vlegs'],4),(f['glegs'],3)):
                for hip,L,tgt in lst: legs.append((offs[si]+jj,par,hip,L,tgt))
    # elbow base angles per key (the motion's own y rotation of the forearms) against hyperextension
    from fk import Motion
    mots={}
    elb=[]
    for si,fs in enumerate(ks):
        for jj,fk in enumerate(fs):
            f=fr[idx[(si,fk)]]
            m=mots.setdefault(f['gm'],Motion(f['gm']))
            elb.append((offs[si]+jj,m.get(14,4,f['gt']),m.get(8,4,f['gt'])))
    return dict(fr=fr,ks=ks,offs=offs,n=n,T=T,hold=hold,legs=legs,elb=elb,name=name,idx=idx,bracket=bracket,
                gd=gd,vd=vd,vT=v.mesh['T'],parts=collision_points(gd),base=base,scale=scale,C=None)

def add_collisions(P, X, size, margin=0.01):
    """Gene points deeper in the victim than the original allows: push-out constraints (kept across rounds)."""
    fr=P['fr']; new=[]
    for i,f in enumerate(fr):
        if not f['touch'] or f['t']%2: continue
        k0,k1,a=P['bracket'](f['stage'],f['t']); O=(X[k0]*(1-a)+X[k1]*a)[None]
        tree=None
        for part,ids in P['parts'].items():
            d=gene_frame_data(fr,P['gd'],np.full(len(ids),i),ids)
            Gp=gene_xform(d,np.repeat(O,len(ids),0))
            # the victim's mesh with its fit applied
            if tree is None:
                vd=P['vd']; dv={'v0':f['vV'],'wv':np.array([lift_w(int(b)) for b in vd])*P['scale'],'vbend':np.isin(vd,list(UPPER)).astype(float),
                                'vpiv':np.repeat(f['vP'][1][None],len(vd),0),'vax':np.repeat(f['vW1'][:,0][None],len(vd),0),'vpos':np.repeat(f['vpos'][None],len(vd),0)}
                Vm=victim_xform(dv,np.repeat(O,len(vd),0),size); tree=cKDTree(Vm); nv=vnormals(Vm,P['vT'])
            dist,j=tree.query(Gp); s=np.einsum('ij,ij->i',Gp-Vm[j],nv[j])
            allowed=P['base'].get((f['stage'],f['t'],part),0.0)+margin
            Tn=near_tris(Vm,P['vT'],Gp); ins=winding(Gp,Vm,Tn)>0.5 if len(Tn) else np.zeros(len(Gp),bool)
            bad=ins&(dist>allowed)
            for q in np.nonzero(bad)[0]:
                new.append((i,int(ids[q]),int(j[q]),nv[j[q]],allowed))
    if P['C'] is None: P['C']=[]
    seen={(c[0],c[1]) for c in P['C']}
    for c in new:
        if (c[0],c[1]) not in seen: P['C'].append(c); seen.add((c[0],c[1]))
    return len(new)

def coll_data(P):
    C=P['C']
    if not C: return None
    fr=P['fr']; I=np.array([c[0] for c in C]); G=np.array([c[1] for c in C]); VJ=np.array([c[2] for c in C])
    d=gene_frame_data(fr,P['gd'],I,G)
    d['v0']=np.array([fr[i]['vV'][vj] for i,vj in zip(I,VJ)]); vb=P['vd'][VJ]
    d['wv']=np.array([lift_w(int(b)) for b in vb])*P['scale']; d['vbend']=np.isin(vb,list(UPPER)).astype(float)
    d['n']=np.array([c[3] for c in C]); d['allowed']=np.array([c[4] for c in C])
    br=[P['bracket'](fr[i]['stage'],fr[i]['t']) for i in I]; d['K0']=np.array([b[0] for b in br]); d['K1']=np.array([b[1] for b in br]); d['A']=np.array([b[2] for b in br])
    return d

def solve(P, lam=(0.5,1.5), mu=4.0, ridge=None, wcoll=4.0, x0=None, use_size=True, max_nfev=150):
    n=P['n']; ks=P['ks']; offs=P['offs']; stages=PUNISH[P['name']]
    if ridge is None: ridge=np.array([0.5,0.5,0.5,2.0,2.0,1.5]+[1.2]*8)
    T=P['T']; C=coll_data(P)
    nx=n*NP+1
    def unpack(x): return x[:n*NP].reshape(n,NP), 1.0+x[n*NP]
    def interp(d,X): A=d['A'][:,None]; return X[d['K0']]*(1-A)+X[d['K1']]*A
    def residuals(x):
        X,sz=unpack(x); r=[]
        OT=interp(T,X); r.append(((gene_xform(T,OT)-victim_xform(T,OT,sz))*T['w'][:,None]).ravel())
        if C is not None:
            OC=interp(C,X); Gp=gene_xform(C,OC); Vp=victim_xform(C,OC,sz)
            s=np.einsum('ij,ij->i',Gp-Vp,C['n']); r.append(np.maximum(-C['allowed']-s,0)*wcoll)
        lg=[]
        for kk,par,hip,L,tgt in P['legs']:
            d_=np.linalg.norm(hip+np.array([0,X[kk,par],0])-tgt)-L*0.995; lg.append(max(0.0,d_)*8)
        r.append(np.array(lg))
        eb=[]
        for kk,bl,br_ in P['elb']:
            for base,add in ((bl,X[kk,10]),(br_,-X[kk,13])):
                if abs(base)>D(10): eb.append(max(0.0,-np.sign(base)*(base+add))*2)
                else: eb.append(0.0)
        r.append(np.array(eb))
        for si,fs in enumerate(ks):
            K=X[offs[si]:offs[si]+len(fs)]
            r.append((np.diff(K,axis=0)/SCALE*lam[0]*0.1).ravel())
            r.append((np.diff(K,n=2,axis=0)/SCALE*lam[1]*0.1).ravel())
            if 'loop' in stages[si]['label']: r.append(((K[0]-K[-1])/SCALE*mu*0.1))
            if si>0 and not stages[si].get('align'): r.append(((K[0]-X[offs[si-1]+len(ks[si-1])-1])/SCALE*mu*0.1))
            r.append((K/SCALE*ridge*0.1).ravel())
        r.append(np.array([(sz-1.0)*0.3]))
        return np.concatenate(r)
    if x0 is None: x0=np.zeros(nx)
    m=len(residuals(x0))
    S=lil_matrix((m,nx),dtype=int); row=0
    def mark(rows,keys,size_col=False):
        for kk in keys: S[rows,kk*NP:(kk+1)*NP]=1
        if size_col: S[rows,n*NP]=1
    for k0,k1 in zip(T['K0'],T['K1']):
        for d_ in range(3): mark(row+d_,[k0,k1],True)
        row+=3
    if C is not None:
        for k0,k1 in zip(C['K0'],C['K1']): mark(row,[k0,k1],True); row+=1
    for kk,par,hip,L,tgt in P['legs']: mark(row,[kk]); row+=1
    for kk,bl,br_ in P['elb']:
        mark(row,[kk]); mark(row+1,[kk]); row+=2
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
    S[row,n*NP]=1; row+=1
    assert row==m,(row,m)
    LOk=np.tile(LO,(n,1)); HIk=np.tile(HI,(n,1))
    for kk,zg,zv,za in P['hold']:
        if zg: LOk[kk,3]=-1e-6; HIk[kk,3]=1e-6
        if zv: LOk[kk,4:6]=-1e-6; HIk[kk,4:6]=1e-6
        if za: LOk[kk,6:]=-1e-6; HIk[kk,6:]=1e-6
    if P.get('fixpose') is not None:           # another body's pose track (same keys): held exactly
        LOk[:,6:]=P['fixpose']-1e-6; HIk[:,6:]=P['fixpose']+1e-6
    if P.get('fixvh') is not None:             # the victim's height given per key (Gorilla on the fat men: elbows to Gene's arms)
        LOk[:,4]=P['fixvh']-1e-6; HIk[:,4]=P['fixvh']+1e-6
    lo=np.append(LOk.ravel(),-0.05 if use_size else -1e-6); hi=np.append(HIk.ravel(),0.05 if use_size else 1e-6)
    x0=np.clip(x0,lo,hi)
    sol=least_squares(residuals,x0,jac_sparsity=S,bounds=(lo,hi),loss='soft_l1',f_scale=0.03,max_nfev=max_nfev,method='trf')
    return sol.x

def fit(name, touches, base, path, scale=1.0, rounds=2, use_size=True):
    P=prepare(name,touches,base,path,scale)
    x=solve(P,use_size=use_size)
    for r in range(rounds):
        X,sz=x[:P['n']*NP].reshape(P['n'],NP),1+x[-1]
        if add_collisions(P,X,sz)==0 and r>0: break
        x=solve(P,x0=x,use_size=use_size)
    X,sz=x[:P['n']*NP].reshape(P['n'],NP),1+x[-1]
    return P,X,sz

def evaluate(P,X,sz):
    T=P['T']; A=T['A'][:,None]; O=X[T['K0']]*(1-A)+X[T['K1']]*A
    e=np.linalg.norm(gene_xform(T,O)-victim_xform(T,O,sz),axis=1)
    O0=np.zeros_like(O); e0=np.linalg.norm(gene_xform(T,O0)-victim_xform(T,O0,1.0),axis=1)
    w=T['w']
    # clipping beyond the original's (mesh test), every 2nd touch frame
    excess=[];excess0=[]
    for i,f in enumerate(P['fr']):
        if not f['touch'] or f['t']%2: continue
        k0,k1,a=P['bracket'](f['stage'],f['t']); Ok=(X[k0]*(1-a)+X[k1]*a)[None]
        vd=P['vd']; dv={'v0':f['vV'],'wv':np.array([lift_w(int(b)) for b in vd])*P['scale'],'vbend':np.isin(vd,list(UPPER)).astype(float),
                        'vpiv':np.repeat(f['vP'][1][None],len(vd),0),'vax':np.repeat(f['vW1'][:,0][None],len(vd),0),'vpos':np.repeat(f['vpos'][None],len(vd),0)}
        for OO,szz,acc in ((Ok,sz,excess),(np.zeros_like(Ok),1.0,excess0)):
            Vm=victim_xform(dv,np.repeat(OO,len(vd),0),szz); tree=cKDTree(Vm); nv=vnormals(Vm,P['vT'])
            worst=0.0
            for part,ids in P['parts'].items():
                d=gene_frame_data(P['fr'],P['gd'],np.full(len(ids),i),ids)
                Gp=gene_xform(d,np.repeat(OO,len(ids),0)); dist,j=tree.query(Gp)
                Tn=near_tris(Vm,P['vT'],Gp); ins=winding(Gp,Vm,Tn)>0.5 if len(Tn) else np.zeros(len(Gp),bool); depth=float(dist[ins].max()) if ins.any() else 0.0
                worst=max(worst,depth-P['base'].get((f['stage'],f['t'],part),0.0))
            acc.append(max(worst,0.0))
    return dict(touch=float(np.average(e,weights=w)),touch90=float(np.percentile(e,90)),touch0=float(np.average(e0,weights=w)),
                clip=float(np.mean(excess)) if excess else 0.0,clipmax=float(np.max(excess)) if excess else 0.0,
                clip0=float(np.mean(excess0)) if excess0 else 0.0,size=float(sz))
