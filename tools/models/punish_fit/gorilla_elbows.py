"""K (Nov 8): the Gorilla Suplex on the standard fat men keeps Gene's original arms; the fat man stands taller so Gene's
arms close round his elbows instead of cutting into his shoulders. Gene's arm touches on the victim's shoulders and
upper arms move to the elbows (the same spot round the arm, at the elbow; snapped to that body's forearm surface);
Gene's pose held at the original's; placement, heights, waist bend and size fitted (fit7 step A)."""
import sys, os, json, numpy as np
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from multiprocessing import Pool
import fit_pose
from fit_pose import OUT, path_of, save, REP
def retarget_elbows(touches):
    """Gene's arm touches only (his body's contacts with the victim's hands pulled the victim down), the ones on the
    victim's shoulders and upper arms moved to the elbows."""
    from fit5 import ARMPART
    out=[]
    for t in touches:
        if t['gp'] not in ARMPART: continue
        if t['gp'] in ARMPART and t['b'] in (5,6,7,11,12,13):
            t=dict(t,b=8 if t['b'] in (5,6,7) else 14,local=[0.0,t['local'][1],t['local'][2]],vp=('R' if t['b'] in (5,6,7) else 'L')+' forearm')
        out.append(t)
    return out
def elbow_heights(P):
    """Per key: how far the victim rises so his elbows are at Gene's elbows (both sides' mean) over the grab (to frame
    34, when the lift starts), easing back to 0 by frame 46 (the throw); 0 at the first key; 3-tap smoothed; -15..+25 cm
    (the fitter's limits)."""
    fs=np.array(P['ks'][0]); grab=34; done=46
    def gap(t):
        f=P['fr'][P['idx'][(0,int(t))]]
        return (f['gP'][8][1]+f['gP'][14][1]-f['vP'][8][1]-f['vP'][14][1])/2
    h=np.array([max(gap(t),0.0) if t<=grab else 0.0 for t in fs])   # taller only (K); the approach's dip left out
    s_=h.copy()
    for j in range(1,len(h)-1):
        if fs[j]<=grab: s_[j]=0.25*h[j-1]+0.5*h[j]+0.25*(h[j+1] if fs[j+1]<=grab else h[j])
    end=s_[fs<=grab][-1]
    for j,t in enumerate(fs):
        if grab<t<=done: s_[j]=end*(done-t)/(done-grab)
        elif t>done: s_[j]=0.0
    s_[0]=0.0
    return np.clip(s_,-0.15,0.25)
def job(a):
    g,s=a
    import fit7
    from fit5 import prepare
    tch,base=fit_pose.touches_for('Gorilla','fat'); tch=retarget_elbows(tch)
    P=prepare('Gorilla',tch,base,path_of(g,'fat' if g==REP['fat'] else None),s); P['flat_allow']=None; P['fixpose']=np.zeros((P['n'],8))
    P['fixvh']=elbow_heights(P)
    X,sz=fit7.step_a(P)
    rep=g==REP['fat']; tag=('rep_Gorilla_fat_%.2f'%s) if rep else ('g_Gorilla_%02d_%.2f'%(g,s))
    m=save(tag,P,X,sz,dict(name='Gorilla',type='fat',group=g,scale=s,rep=rep,pose_from='original (K: arms round the elbows)'))
    return 'Gorilla g%02d %.2f touch %.1f -> %.1f cm, clip %s, size %.3f, victim height %.0f..%.0f cm (keys: %s)'%(g,s,m['e0']*100,m['e1']*100,
        {k:'%.1f'%(v[1]*100) for k,v in m['clip1'].items()},sz,X[:,4].min()*100,X[:,4].max()*100,' '.join('%.0f'%(v*100) for v in X[:,4]))
if __name__=='__main__':
    jobs=[(g,s) for s in (1.0,1.3) for g in (2,3,7,8,20)]+[(21,1.3)]
    if sys.argv[1:]: jobs=[j for j in jobs if j[1]==float(sys.argv[1])]
    with Pool(2) as p:
        for line in p.imap_unordered(job,jobs): print(line,flush=True)
