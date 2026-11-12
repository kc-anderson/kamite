"""K (Nov 12, Dr. Ion at 1.29): Spanking on the human-sized bodies at enemy size 1.30, the fat men's way: the game's own
placement (no placement curves) and Gene's holding (left) arm straight across the back, palm down (spank_fat2's
STRAIGHT / TWIST, eased in over the grab's first 8 frames, out from frame 12 of the finish to the launch at 21), raised to
the lowest angle where his forearm clears the back (deepest point <= 0.3 cm, median over the held frames). Replaces
the fit7 1.30 Spanking fits (off: 17-20 cm), which left these bodies their own punish past the 12% pair scaling.

    GODHAND_EXTRACT=<extract> python -I spank130.py [group ...]    (fits7/g_Spanking_<g>_1.30, then emit7)"""
import sys, os, json, numpy as np
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
import fit_pose
from fit_pose import path_of, save, TYPES, REP, type_of
from spank_fat2 import key_weights, STRAIGHT, TWIST
D=np.radians
SCALE=1.3
def groups():
    return [g for t in ('men','tall','small') for g in TYPES[t]]
def prepare_g(g):
    import fit5
    t=type_of(g); tch,base=fit_pose.touches_for('Spanking',t)
    P=fit5.prepare('Spanking',tch,base,path_of(g,t if g==REP[t] else None),SCALE)
    P['parts']={'L arm':P['parts']['L arm']}; P['flat_allow']={'L arm'}
    return P
def solve(P):
    import fit7, fit5
    from fit5 import gene_xform
    X=np.zeros((P['n'],fit5.NP)); ctx=fit7.frame_ctx(P,X,1.0); W=key_weights(P)
    held=[(i,c) for i,c in ctx.items() if c['grid'] is not None and W[c['k0']]*(1-c['a'])+W[c['k1']]*c['a']>=1.0]
    def pose(th): O=np.zeros(fit5.NP); O[8:11]=th; return O
    def forearm(th):
        out=[]
        for i,c in held:
            ids,dd=c['cps']['L arm']; m=dd['lfore']>0.5
            if m.any(): out.append(c['grid'](gene_xform(dd,np.repeat(pose(th)[None],len(ids),0)))[m].max())
        return np.median(out)
    swing,elbow=D(STRAIGHT[0]),D(STRAIGHT[1])
    r=next((r for r in np.arange(0.0,86.0) if forearm(np.array([D(r),swing,elbow]))<=0.003),85.0)
    th=np.array([D(r),swing,elbow]); Xb=X.copy(); Xb[:,8:11]=th[None]*W[:,None]
    return Xb,th
def job(g):
    P=prepare_g(g); X,th=solve(P); t=type_of(g)
    m=save('g_Spanking_%02d_%.2f'%(g,SCALE),P,X,1.0,dict(name='Spanking',type=t,group=g,scale=SCALE,rep=False,own_placement=True,
                                                     pose_from='own (K: the fat men\'s straight arm, game placement)',ltwist=TWIST))
    return 'Spanking %s g%02d at %.2f: raise %.0f deg; L arm clip %.1f cm'%(t,g,SCALE,np.degrees(th[0]),m['clip1']['L arm'][1]*100)
if __name__=='__main__':
    from multiprocessing import Pool
    gs=[int(a) for a in sys.argv[1:] if a.isdigit()] or groups()
    with Pool(2) as p:
        for line in p.imap_unordered(job,gs): print(line,flush=True)
