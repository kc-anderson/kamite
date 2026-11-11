"""Cobra Twist's release (victim 1547) on bodies other than its own (K, Nov 11: "after the release they stumble forward and
stretch their neck, which lifts them off the ground"; the tall bodies, Cobra's own, don't). The motions key the hips at
the tall body's height; a shorter body's legs can't reach the leg IK's targets, so it hangs from stretched legs. The fits
lower the victim through the grab and pulls (to the -15 cm limit) but leave the release at 0. This sets the release's
victim height per key to the least lowering that lets both legs reach their targets (bend kept).

    GODHAND_EXTRACT=<extract> python -I release_reach.py [--check]    (rewrites resources/punish_fit.txt in place)"""
import sys, os, re, json, math, numpy as np
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
import sim
from scene import EX
from fit_pose import TYPES, REP, REPFILE
B=json.load(open(os.path.join(HERE,'bodies.json')))
FILE=os.path.join(HERE,'..','..','..','resources','punish_fit.txt')
def group_of(keys):
    for g,b in enumerate(B):
        if keys[0] in b['keys']: return g
def path_of(g):
    for t,gs in TYPES.items():
        if g==REP.get(t): return EX+'/em/%s.dat'%REPFILE[t]
    return EX+'/em/%s.dat'%B[g]['files'][0]
def needed(path,scale,frames):
    """per release frame: the hips' lowering (m, <= 0, before the fit's size=) that lets both legs reach."""
    fr=sim.simulate('Cobra',path,scale)
    rel=[f for f in fr if f['stage']==3]; out={}
    for t in frames:
        f=rel[min(t,len(rel)-1)]; h=0.0
        for hip,L,tgt in f['vlegs']:
            L=L*scale*0.995; d=hip-tgt; hz2=d[0]**2+d[2]**2
            top=math.sqrt(max(L*L-hz2,0.0))     # the hip's highest point above the target the leg reaches
            h=min(h,top-d[1])
        out[t]=h/scale   # the file's heights are the model's own (the runtime adds them to bone 0; the actor's scale applies)
    return out
if __name__=='__main__':
    if '--check' in sys.argv:   # the tall body (Cobra's own) should need ~0
        print('em07', {t:round(v*100,1) for t,v in needed(EX+'/em/em07.dat',1.0,range(0,25,4)).items()}); sys.exit()
    L=open(FILE).read().split('\n'); cur=None; done=[]; cache={}
    for i,l in enumerate(L):
        if l.startswith('fit '):
            p=l.split('#')[0].split(); cur=None
            if p[1]=='CobraTwist' and 'off' not in p:
                g=group_of(p[2].split(',')); sc=float(re.search(r'scale=([\d.]+)',l).group(1))
                if g is not None and os.path.exists(path_of(g)): cur=(g,sc)
        elif cur and l.startswith('victim 1547 '):
            keys=[k.split(':') for k in l.split()[2:]]; last=max(int(k[0]) for k in keys)
            fr=sorted(set(range(0,last+1,2))|{last})     # every 2nd frame: a stepping foot's target moves fast
            h=cache.get(cur) or cache.setdefault(cur,needed(path_of(cur[0]),cur[1],fr))
            L[i]='victim 1547 '+' '.join('%d:%.3f,0.00'%(t,h[t]) for t in fr)
            done.append('g%02d %.2f: %s'%(cur[0],cur[1],' '.join('%.0f'%(h[t]*100) for t in fr)))
    open(FILE,'w').write('\n'.join(L)); print('\n'.join(done))
