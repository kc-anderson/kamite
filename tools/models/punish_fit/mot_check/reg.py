import os
ROOT=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),'../../../..'))
sys_path_here=os.path.dirname(os.path.abspath(__file__))
import sys, subprocess, numpy as np
from scipy.spatial.transform import Rotation as Rot, Slerp
sys.path.insert(0,ROOT+'/tools/models'); sys.path.insert(0,ROOT+'/tools/models/punish_fit')
from mot import Mot
from common import motion
sys.path.insert(0,sys_path_here)
from trk import load, at
def R(e): return Rot.from_euler('ZYX',[e[2],e[1],e[0]])
def wob(m,bone,n):
    out=[]
    for t in np.arange(0.5,n-1,1.0):
        a=R(m.pose(np.floor(t),30)[bone][1]); b=R(m.pose(np.ceil(t),30)[bone][1])
        mid=Slerp([0,1],Rot.concatenate([a,b]))(0.5); out.append(np.degrees((mid.inv()*R(m.pose(t,30)[bone][1])).magnitude()))
    return np.array(out)
lines=open(ROOT+'/resources/punish_fit.txt').read().split('\n')
seen=set(); worst=[]
for l in lines:
    if not l.startswith('fit '): continue
    p=l.split(); key=(p[1],p[2].split(',')[0])
    if key in seen: continue
    seen.add(key)
    try: tr=load(key[1],key[0])
    except StopIteration: continue
    for mi,keys in tr.items():
        V=np.array([at(keys,f) for f in range(Mot(motion(mi)[1]).frame_count)])
        if not np.any(np.abs(V[:,4:])>0.05): continue
        t,blob=motion(mi); a=Mot(blob); n=a.frame_count
        np.savetxt('vals.txt',V); open('src.bin','wb').write(blob)
        subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)),'apply_fit'),'src.bin','out.bin','vals.txt'],check=True); b=Mot(open('out.bin','rb').read())
        for bone in (1,7,8,13,14,15,16):
            wo,wn=wob(a,bone,n),wob(b,bone,n); ex=wn-wo; k=int(np.argmax(ex))
            worst.append((ex[k],key[0],key[1],mi,bone,k+0.5,wo[k],wn[k]))
worst.sort(reverse=True)
print(len(seen),'fits;',len(worst),'bone-motion checks; worst new wobble beyond the original:')
for w in worst[:10]: print('  +%.1f deg  %s %s motion %d bone %d at %.1f (own %.1f, new %.1f)'%w)
