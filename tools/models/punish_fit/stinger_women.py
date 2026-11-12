"""Stinger on the women (K, Nov 12): the original's wide-kneed squat replaced by knees and thighs together, the seat
tipped up 30 deg and the hips 5 cm higher with the back straighter; the feet stay planted and the head stays where
Gene holds it (the waist bend and the hips' backward shift solved per body so bone 4 keeps its place through the loop).
Eased in over the grab (805), held through the mash loop (807); in the slam (809) the hips / seat / back ease back to the
game's over its first 16 frames as Gene lifts her, the knees stay together until she lies down (frames 60-100).
K picked variant "B with the 30 deg tilt" from renders.

    GODHAND_EXTRACT=<extract> python -I stinger_women.py    (rewrites resources/punish_fit.txt in place; re-run after emit7)

Adds to each women's Stinger fit's victim keys: height += raise, bend += the solved bend, and the 3rd-5th values
(hips back m, pelvis tilt deg, legs 0..1; punish_fit.cpp). Idempotent: keys that already carry them are recomputed from
their base."""
import sys, os, re, json, math, numpy as np
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from scipy.optimize import least_squares
from common import skeleton
from scene import EX
import fk
from fk import Motion
B=json.load(open(os.path.join(HERE,'bodies.json')))
FILE=os.path.join(HERE,'..','..','..','resources','punish_fit.txt')
TILT=30.0; RAISE=0.05; FEET_IN=0.6   # FEET_IN = punish_fit.cpp kFeetIn
def ramp(a,b,t): return min(max((t-a)/(b-a),0.0),1.0)
def weights(m,t):
    """(legs, pose) weight at victim motion m frame t."""
    if m==805: r=ramp(0,9,t); return r,r
    if m==809: return 1.0-ramp(60,100,t), 1.0-ramp(0,16,t)
    return 1.0,1.0
class Edited:
    def __init__(s,mot,legs,raise_,back,tilt,bend): s.m=mot; s.frames=mot.frames; s.e=(legs,raise_,back,tilt,bend)
    def has(s,b,c): return s.m.has(b,c)
    def get(s,b,c,t,default=0.0):
        legs,raise_,back,tilt,bend=s.e; v=s.m.get(b,c,t,default)
        if (b,c)==(0,1): v+=raise_
        if (b,c)==(0,2): v-=back
        if (b,c)==(1,3): v+=bend
        if (b,c)==(17,3): v+=tilt
        if b in (18,22) and c==4: v*=1-legs
        if b in (20,24) and c==0: v*=1-FEET_IN*legs
        return v
def solve(path):
    """The bend (rad) and hips back (m) that keep the head (bone 4) in place through the loop with the seat tipped up."""
    sk=skeleton(path); M=Motion(807); ts=range(0,M.frames,4)
    base=[fk.pose(sk,M,t)[1][4] for t in ts]
    def res(x): return np.concatenate([fk.pose(sk,Edited(M,1.0,RAISE,x[1],math.radians(TILT),x[0]),t)[1][4]-b for t,b in zip(ts,base)])
    r=least_squares(res,[0.1,0.0]); return r.x[0],r.x[1],np.abs(r.fun).max()
def women_groups():
    return [g for g,b in enumerate(B) if b.get('woman')]
if __name__=='__main__':
    keys={}
    for g in women_groups():
        for k in B[g]['keys']: keys[k]=g
    sol={}; L=open(FILE).read().split('\n'); cur=None; done=[]
    for i,l in enumerate(L):
        if l.startswith('fit '):
            p=l.split('#')[0].split(); cur=None
            if p[1]=='Stinger' and 'off' not in p:
                gs={keys.get(k) for k in p[2].split(',')}-{None}
                if gs:
                    g=gs.pop()
                    if g not in sol:
                        path=EX+'/em/%s.dat'%('em16' if g==1 else B[g]['files'][0])
                        sol[g]=solve(path); done.append('g%02d: bend %+.1f deg, hips back %.1f cm (head within %.1f cm)'%(g,math.degrees(sol[g][0]),sol[g][1]*100,sol[g][2]*100))
                    cur=g
        elif cur is not None and l.startswith('victim '):
            m=int(l.split()[1])
            if m not in (805,807,809): continue
            bend,back,_=sol[cur]; out=[]
            for item in l.split()[2:]:
                f,vals=item.split(':'); f=int(f); v=[float(x) for x in vals.split(',')]
                wl,wp=weights(m,f)
                if len(v)>=5:   # already carried: back to the fit's own values
                    v=[v[0]-wp*RAISE,v[1]-wp*math.degrees(bend)]   # same weights as when they were added
                v=v[:2]
                out.append('%d:%.3f,%.2f,%.3f,%.1f,%.2f'%(f,v[0]+wp*RAISE,v[1]+wp*math.degrees(bend),wp*back,wp*TILT,wl))
            L[i]='victim %d %s'%(m,' '.join(out))
    open(FILE,'w').write('\n'.join(L)); print('\n'.join(done))
