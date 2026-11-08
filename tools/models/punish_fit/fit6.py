"""Fitter v6: v5's model, solved in two steps.
A: placement, heights, victim bend and size against the touches (fit5.solve with the pose angles held at 0).
B: Gene's torso and arms per key frame against the victim's mesh as posed by A: the arm touches kept, Gene's points
   no deeper in the victim than the original allows (+1 cm), small angles preferred; warm-started frame to frame,
   then smoothed over time (loops wrap; zero where the pose must ease out)."""
import numpy as np, math
from scipy.optimize import minimize
from scipy.spatial import cKDTree
from fit5 import *
from punishes import PUNISH
POSE=list(range(6,14))
def step_a(P, use_size=True, rounds=2):
    import fit5
    LOs=fit5.LO; HIs=fit5.HI
    fit5.LO=fit5.LO.copy(); fit5.HI=fit5.HI.copy(); fit5.LO[6:]=-1e-6; fit5.HI[6:]=1e-6
    x=solve(P,use_size=use_size)
    parts=P['parts']; P['parts']={'torso':parts['torso']}   # Gene's body: placement and heights keep it out
    for r in range(rounds):
        X,sz=x[:P['n']*NP].reshape(P['n'],NP),1+x[-1]
        if add_collisions(P,X,sz)==0: break
        x=solve(P,x0=x,use_size=use_size,wcoll=12.0)
    P['parts']=parts; P['C']=None
    fit5.LO=LOs; fit5.HI=HIs
    return x[:P['n']*NP].reshape(P['n'],NP),1+x[-1]

def victim_mesh(P,f,O,sz):
    vd=P['vd']; dv={'v0':f['vV'],'wv':np.array([lift_w(int(b)) for b in vd])*P['scale'],'vbend':np.isin(vd,list(UPPER)).astype(float),
                    'vpiv':np.repeat(f['vP'][1][None],len(vd),0),'vax':np.repeat(f['vW1'][:,0][None],len(vd),0),'vpos':np.repeat(f['vpos'][None],len(vd),0)}
    return victim_xform(dv,np.repeat(O[None],len(vd),0),sz)

def step_b(P, X, sz, wt=1.0, wc=6.0, wr=0.25, near=2):
    fr=P['fr']; ks=P['ks']; offs=P['offs']; T=P['T']; idx=P['idx']
    Xb=X.copy()
    lo=LO[POSE]; hi=HI[POSE]
    # touches by frame
    tf=[ (fr[i]['stage'],fr[i]['t']) for i in T['I'] ]
    arm=np.array([w>=1.0 for w in T['w']])
    prev=np.zeros(len(POSE))
    for si,fs in enumerate(ks):
        tw=PUNISH[P['name']][si].get('touch')
        for jj,fk in enumerate(fs):
            kk=offs[si]+jj
            held=[h for h in P['hold'] if h[0]==kk][0][3]
            if held or tw is None: Xb[kk,POSE]=0; prev=np.zeros(len(POSE)); continue
            f=fr[idx[(si,fk)]]; O0=X[kk].copy()
            Vm=victim_mesh(P,f,O0,sz); tree=cKDTree(Vm)
            from winding import winding, near_tris
            allp=np.concatenate([fr[idx[(si,fk)]]['gV'][ids] for ids in P['parts'].values()]); Tn=near_tris(Vm,P['vT'],allp,0.5)
            i=idx[(si,fk)]
            cps={part:(ids,gene_frame_data(fr,P['gd'],np.full(len(ids),i),ids)) for part,ids in P['parts'].items()}
            # touches within +-near frames, arm ones only, evaluated at this key's frame data
            sel=[q for q,(s_,t_) in enumerate(tf) if s_==si and abs(t_-fk)<=near and arm[q]]
            if sel:
                d=gene_frame_data(fr,P['gd'],np.full(len(sel),i),T_G[np.array(sel)])
                tgt=victim_targets(P,T,np.array(sel),O0,sz)
            def cost(th):
                O=O0.copy(); O[POSE]=th; c=wr*np.sum((th/np.radians(15))**2)
                if sel:
                    G=gene_xform(d,np.repeat(O[None],len(sel),0)); c+=wt*np.mean(np.sum((G-tgt)**2,1))/0.0009
                for part,(ids,dd) in cps.items():
                    G=gene_xform(dd,np.repeat(O[None],len(ids),0)); dist,j=tree.query(G)
                    ins=winding(G,Vm,Tn)>0.5 if len(Tn) else np.zeros(len(G),bool)
                    allowed=P['base'].get((si,fk,part),0.0)+0.01
                    ex=np.where(ins,dist-allowed,0.0); c+=wc*np.sum(np.maximum(ex,0)**2)/0.0009/len(ids)*10
                return c
            best=minimize(cost,np.clip(prev,lo,hi),method='Powell',bounds=list(zip(lo,hi)),options=dict(maxiter=3000,xtol=1e-3,ftol=1e-4))
            Xb[kk,POSE]=best.x; prev=best.x
    # smooth over time per stage (3-tap), loops wrap
    for si,fs in enumerate(ks):
        o=offs[si]; K=Xb[o:o+len(fs),POSE].copy()
        loop='loop' in PUNISH[P['name']][si]['label']
        S=K.copy()
        for j in range(len(fs)):
            a=K[(j-1)%len(fs)] if (loop or j>0) else K[j]; b=K[(j+1)%len(fs)] if (loop or j<len(fs)-1) else K[j]
            S[j]=0.25*a+0.5*K[j]+0.25*b
        for j in range(len(fs)):
            if [h for h in P['hold'] if h[0]==o+j][0][3]: S[j]=0
        Xb[o:o+len(fs),POSE]=S
    return Xb

T_G=None
def victim_targets(P,T,sel,O,sz):
    d={k:T[k][sel] for k in ('v0','wv','vbend','vpiv','vax','vpos')}
    return victim_xform(d,np.repeat(O[None],len(sel),0),sz)

def fit(name,touches,base,path,scale=1.0,use_size=True):
    global T_G
    P=prepare(name,touches,base,path,scale)
    from fit import map_points
    # Gene vertex ids of the touch items (prepare kept them in order)
    T_G=P['T']['G']
    X,sz=step_a(P,use_size)
    Xb=step_b(P,X,sz)
    return P,Xb,sz
