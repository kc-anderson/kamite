"""Batch for fit7 (Nov 8, K: poses per body type, Gene's clipping no worse than the original's).
Per punish and body type: the type's representative body gets the full fit (placement, heights, bend, size, Gene's
torso and arms); every other body of the type gets placement / heights / bend / size with the representative's pose
track held. Two enemy scales (1.0 and 1.3): the runtime interpolates by the enemy's size. At 1.0 the original pairing's
type keeps Gene's original pose (and the original body itself stays untouched); at 1.3 it is fitted like the rest.
Cobra Twist on women and fat men: Gene's arm touches go round the neck (K). Gorilla Suplex on the fat men: Gene's arms
no deeper in the victim than 1 cm, the original's own clipping not allowed (K: his arms fold in too much)."""
import sys, os, json, time, numpy as np
from multiprocessing import Pool
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
OUT=os.environ.get('PUNISH_FIT_FITS7',os.path.join(HERE,'fits7')); os.makedirs(OUT,exist_ok=True)
B=json.load(open(os.path.join(HERE,'bodies.json')))
TYPES={'men':[0,6,9,10,12,13,14,15,22],'women':[1,16,19],'fat':[2,3,7,8,17,20,21],'tall':[4,11],'small':[5,18]}
REPFILE={'men':'em00','women':'em16','fat':'em18','tall':'em07','small':'em11'}
REP={'men':0,'women':1,'fat':2,'tall':11,'small':5}
REF={'Suplex':'em00','Stinger':'em18','Cobra':'em07','Spanking':'em16','Gorilla':'em79'}
REFTYPE={'Suplex':'men','Stinger':'fat','Cobra':'tall','Spanking':'women','Gorilla':'fat'}
REFGROUP={'Suplex':0,'Stinger':2,'Cobra':11,'Spanking':1,'Gorilla':21}
SCALES=(1.0,1.3)
def type_of(g): return next(t for t,gs in TYPES.items() if g in gs)
def wanted(name,t):
    if name=='Spanking' and t=='fat': return False      # K: if it can't be made to work, we don't do it
    if name=='Suplex' and t=='fat': return False        # plays the Gorilla Suplex there
    if name=='Gorilla': return t=='fat'
    return True
def groups_of(name,t):
    gs=list(TYPES[t])
    if name=='Gorilla': gs=[g for g in gs if g!=17]     # em60: no Gorilla Suplex
    return gs
def path_of(g,t=None):
    f=REPFILE[t] if (t and g==REP[t]) else B[g]['files'][0]
    return os.path.join(EXDIR(),'em','%s.dat'%f)
def EXDIR():
    from common import EX; return EX
def touches_for(name,t):
    import fit7
    from fit5 import reference_data, retarget_neck
    tch,base=reference_data(name,os.path.join(EXDIR(),'em','%s.dat'%REF[name]))
    if name=='Cobra' and t in ('women','fat'): tch=retarget_neck(tch)
    return tch,base
def flat_for(name): return {'L arm','R arm'} if name=='Gorilla' else None
def metrics(P,X,sz):
    import fit7
    from fit5 import gene_xform, victim_xform
    T=P['T']; A=T['A'][:,None]; O=X[T['K0']]*(1-A)+X[T['K1']]*A
    e=np.linalg.norm(gene_xform(T,O)-victim_xform(T,O,sz),axis=1); O0=np.zeros_like(O)
    e0=np.linalg.norm(gene_xform(T,O0)-victim_xform(T,O0,1.0),axis=1); w=T['w']
    c1=fit7.parts_excess(P,X,sz); c0=fit7.parts_excess(P,np.zeros_like(X),1.0)
    return dict(e0=float(np.average(e0,weights=w)),e1=float(np.average(e,weights=w)),e1p90=float(np.percentile(e,90)),
                clip0={k:v for k,v in c0.items()},clip1={k:v for k,v in c1.items()},size=float(sz))
def save(tag,P,X,sz,extra):
    m=metrics(P,X,sz); m.update(extra); m['ks']=P['ks']
    np.save(os.path.join(OUT,tag+'.npy'),X); json.dump(m,open(os.path.join(OUT,tag+'.json'),'w'))
    return m
def rep_job(a):
    name,t,s=a; t0=time.time()
    import fit7
    tch,base=touches_for(name,t)
    P,X,sz=fit7.fit(name,tch,base,path_of(REP[t],t),scale=s,flat_allow=flat_for(name))
    m=save('rep_%s_%s_%.2f'%(name,t,s),P,X,sz,dict(name=name,type=t,group=REP[t],scale=s,rep=True))
    return 'rep %-8s %-6s %.2f  touch %.1f -> %.1f cm  clip %s  %.0fs'%(name,t,s,m['e0']*100,m['e1']*100,
           {k:'%.1f/%.1f'%(v[0]*100,v[1]*100) for k,v in m['clip1'].items()},time.time()-t0)
def group_job(a):
    name,g,t,s=a; t0=time.time()
    import fit7
    from fit5 import prepare
    tch,base=touches_for(name,t)
    P=prepare(name,tch,base,path_of(g),s); P['flat_allow']=flat_for(name)
    rp=os.path.join(OUT,'rep_%s_%s_%.2f.npy'%(name,t,s))
    P['fixpose']=np.load(rp)[:,6:] if os.path.exists(rp) else np.zeros((P['n'],8))
    X,sz=fit7.step_a(P)
    m=save('g_%s_%02d_%.2f'%(name,g,s),P,X,sz,dict(name=name,type=t,group=g,scale=s,rep=False,pose_from=os.path.basename(rp) if os.path.exists(rp) else 'original'))
    return 'grp %-8s g%02d %-6s %.2f  touch %.1f -> %.1f cm  clip %s  %.0fs'%(name,g,t,s,m['e0']*100,m['e1']*100,
           {k:'%.1f/%.1f'%(v[0]*100,v[1]*100) for k,v in m['clip1'].items()},time.time()-t0)
def fix_job(tag):
    """A body whose clipping with its representative's pose is past 2.5 cm: its own pose, refined from that one."""
    t0=time.time()
    import fit7
    from fit5 import prepare
    m=json.load(open(os.path.join(OUT,tag+'.json'))); X=np.load(os.path.join(OUT,tag+'.npy'))
    name,g,t,s=m['name'],m['group'],m['type'],m['scale']
    tch,base=touches_for(name,t)
    P=prepare(name,tch,base,path_of(g),s); P['flat_allow']=flat_for(name); sz=m['size']
    before=max(v[1] for v in m['clip1'].values())
    Xb=fit7.refine(P,X,X.copy(),sz,fit7.frame_ctx(P,X,sz),sweeps=2,grid_seeds=False)
    m2=save(tag,P,Xb,sz,dict(name=name,type=t,group=g,scale=s,rep=False,pose_from='own (refined from the representative)'))
    return 'fix %-8s g%02d %-6s %.2f  clip %.1f -> %.1f cm  touch %.1f -> %.1f cm  %.0fs'%(name,g,t,s,before*100,
           max(v[1] for v in m2['clip1'].values())*100,m['e1']*100,m2['e1']*100,time.time()-t0)
def plan(which, scales, only=None):
    reps=[]; grps=[]
    for name in which:
        for t in TYPES:
            if not wanted(name,t) or (only is not None and (name,t) not in only): continue
            for s in scales:
                orig=(t==REFTYPE[name] and s==1.0 and name!='Gorilla')
                if not orig: reps.append((name,t,s))
                for g in groups_of(name,t):
                    if s==1.0 and g==REFGROUP[name]: continue             # the original pairing
                    if g==REP[t] and not orig: continue                    # the representative's own fit
                    grps.append((name,g,t,s))
    reps=[r for r in reps if not os.path.exists(os.path.join(OUT,'rep_%s_%s_%.2f.json'%r))]
    grps=[r for r in grps if not os.path.exists(os.path.join(OUT,'g_%s_%02d_%.2f.json'%(r[0],r[1],r[3])))]
    return reps,grps
if __name__=='__main__':
    which=['Suplex','Stinger','Cobra','Spanking','Gorilla']
    if sys.argv[1:2]==['--fix']:
        tags=[]
        for f in sorted(os.listdir(OUT)):
            if f.startswith('g_') and f.endswith('.json'):
                m=json.load(open(os.path.join(OUT,f)))
                if m.get('pose_from','').startswith('own') or m['e1']>=0.12 or m['group']==10: continue   # g10 (em75/76): open extra geometry, the clip test is unreliable
                if max(v[1] for v in m['clip1'].values())>0.025: tags.append(f[:-5])
        print(len(tags),'bodies to refine',flush=True); t0=time.time()
        with Pool(2) as p:
            for line in p.imap_unordered(fix_job,tags): print(line,' [%.0fs]'%(time.time()-t0),flush=True)
        sys.exit(0)
    if sys.argv[1:2]==['--mid']:
        # a fit at 1.15 where the 1.3 one can't be made to work, so mid-size enemies still get the pairing
        from emit7 import verdict
        only=set()
        for f in os.listdir(OUT):
            if f.startswith('rep_') and f.endswith('_1.30.json'):
                m=json.load(open(os.path.join(OUT,f)))
                if verdict(m)[0]=='off': only.add((m['name'],m['type']))
        print('mid-size fits for',sorted(only),flush=True)
        reps,grps=plan(which,(1.15,),only)
    else:
        reps,grps=plan(sys.argv[1:] or which,SCALES)
    print(len(reps),'representative fits,',len(grps),'body fits',flush=True); t0=time.time()
    with Pool(2) as p:
        for line in p.imap_unordered(rep_job,reps): print(line,' [%.0fs]'%(time.time()-t0),flush=True)
        for line in p.imap_unordered(group_job,grps): print(line,' [%.0fs]'%(time.time()-t0),flush=True)
