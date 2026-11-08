"""K (Nov 8): Spanking on the fat men (the other fat bodies: em18's arm shape, raised until the forearm clears the back), the game's own placement, Gene's holding (left) arm raised onto the back.
One raise / swing / elbow per body, held through the hold and the spanks (eased in over the grab's first 8 frames,
out from frame 12 of the finish to the launch at 21): a per-key fit made the arm flail. The arm no deeper in the victim
than 1 cm where it can be, its hand on its touch spots (the original's, snapped to this body's back)."""
import sys, os, json, numpy as np
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from multiprocessing import Pool
from scipy.optimize import minimize
import fit_pose
from fit_pose import OUT, path_of, save, TYPES
D=np.radians
K_EXTRA_RAISE=np.radians(10.0)   # on top of em18's fit, K's eye (Nov 8)
STRAIGHT=(0.0,45.0)   # the other fat bodies: swing, elbow opening (deg); K: "stretched out and across the back"
def key_weights(P):
    w=[]
    for si,fs in enumerate(P['ks']):
        for f in fs:
            if si==0: w.append(min(1.0,f/8.0))
            elif si==1: w.append(1.0)
            else: w.append(1.0 if f<=12 else max(0.0,(21-f)/9.0))
    return np.array(w)
def job(g):
    import fit7, fit5
    from fit5 import prepare, gene_xform, victim_xform
    tch,base=fit_pose.touches_for('Spanking','fat')
    P=prepare('Spanking',tch,base,path_of(g,'fat' if g==fit_pose.REP['fat'] else None),1.0)
    P['parts']={'L arm':P['parts']['L arm']}; P['flat_allow']={'L arm'}
    X=np.zeros((P['n'],fit5.NP)); ctx=fit7.frame_ctx(P,X,1.0); W=key_weights(P)
    frames=[(i,c) for i,c in ctx.items() if c['grid'] is not None]
    T=P['T']; sel=np.nonzero((T['w']>=1.0)&(T['larm']>0))[0]
    ds={k:v[sel] for k,v in T.items() if isinstance(v,np.ndarray) and len(v)==len(T['I'])}
    wsel=W[T['K0'][sel]]*(1-T['A'][sel])+W[T['K1'][sel]]*T['A'][sel]
    def pose(th,w):
        O=np.zeros(fit5.NP); O[8:11]=th*w; return O
    def cost(th):
        worst=0.0; msq=0.0
        for i,c in frames:
            w=W[c['k0']]*(1-c['a'])+W[c['k1']]*c['a']
            if w<=0: continue
            ids,dd=c['cps']['L arm']
            ex=np.maximum(c['grid'](gene_xform(dd,np.repeat(pose(th,w)[None],len(ids),0)))-c['allow']['L arm'],0.0)
            worst=max(worst,ex.max()); msq+=np.mean(ex**2)
        c_=4*((worst/0.01)**2+10*msq/len(frames)/1e-4)
        O=np.zeros((len(sel),fit5.NP)); O[:,8:11]=th[None]*wsel[:,None]
        c_+=np.mean(np.sum((gene_xform(ds,O)-victim_xform(ds,O,1.0))**2,1))/0.0009
        return c_+0.3*np.sum((th/D(30))**2)
    if g==fit_pose.REP['fat']:
        # em18: fitted (0976373, K: "almost perfect, just needs to come up a little bit more") + K_EXTRA_RAISE (3a23805).
        # K (Nov 8, in play): the first fit swung the arm forward (-60 deg) so it lay ALONG the back and showed more;
        # it should rotate up and round, ACROSS the back: the swing kept near 0 (negative = forward), raise and elbow free
        lo=np.array([D(-20),D(-5),D(-70)]); hi=np.array([D(80),D(30),D(60)])
        best=None
        for r in (0,30,60):
            for s_ in (0,15,30):
                for e in (-40,0,30):
                    th=D(np.array([r,s_,e],float)); v=cost(th)
                    if best is None or v<best[0]: best=(v,th)
        res=minimize(cost,best[1],method='Powell',bounds=list(zip(lo,hi)),options=dict(maxiter=2000,xtol=1e-3,ftol=1e-5))
        th=np.clip(res.x,lo,hi)
        th[0]=min(th[0]+K_EXTRA_RAISE,D(85))
    else:
        # K (Nov 8, human Elvis): the bodies' own fits lifted the arm off the back (70-85 deg, elbow straightened);
        # em18's bent shape raised until it cleared (7f3a7f9) twisted the shoulder with the elbow bent at an awkward
        # angle. K: "his arm simply stretched out and across the back". So: swing 0, the elbow opened 45 deg (from the
        # motion's ~-70), raised to the lowest angle where Gene's forearm clears the back (its deepest point <= 0.3 cm,
        # median over the held frames). Rendered from behind the victim (yaw 175, pitch 8: matches K's screenshots).
        held=[(i,c) for i,c in frames if W[c['k0']]*(1-c['a'])+W[c['k1']]*c['a']>=1.0]
        def forearm(th):
            out=[]
            for i,c in held:
                ids,dd=c['cps']['L arm']; m=dd['lfore']>0.5
                if m.any(): out.append(c['grid'](gene_xform(dd,np.repeat(pose(th,1.0)[None],len(ids),0)))[m].max())
            return np.median(out)
        swing,elbow=D(STRAIGHT[0]),D(STRAIGHT[1])
        r=next((r for r in np.arange(0.0,86.0) if forearm(np.array([D(r),swing,elbow]))<=0.003),85.0)
        th=np.array([D(r),swing,elbow])
    Xb=X.copy(); Xb[:,8:11]=th[None]*W[:,None]
    rep=g==fit_pose.REP['fat']; tag=('rep_Spanking_fat_1.00') if rep else ('g_Spanking_%02d_1.00'%g)
    m=save(tag,P,Xb,1.0,dict(name='Spanking',type='fat',group=g,scale=1.0,rep=rep,pose_from='own (K: the left arm on the back, held)'))
    m0=fit7.parts_excess(P,X,1.0)
    return 'Spanking fat g%02d: raise %.0f swing %.0f elbow %.0f deg; L arm deeper than the original %.1f -> %.1f cm max (%.1f -> %.1f mean)'%(
        g,*np.degrees(th),m0['L arm'][1]*100,m['clip1']['L arm'][1]*100,m0['L arm'][0]*100,m['clip1']['L arm'][0]*100)
if __name__=='__main__':
    # --all refits em18 too (K tests its pose: left as is by default)
    groups=[g for g in TYPES['fat'] if '--all' in sys.argv or g!=fit_pose.REP['fat']]
    with Pool(2) as p:
        for line in p.imap_unordered(job,groups): print(line,flush=True)
