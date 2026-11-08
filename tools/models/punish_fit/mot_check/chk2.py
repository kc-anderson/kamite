import os
ROOT=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),'../../../..'))
sys_path_here=os.path.dirname(os.path.abspath(__file__))
import sys, subprocess, numpy as np
sys.path.insert(0,ROOT+'/tools/models'); sys.path.insert(0,ROOT+'/tools/models/punish_fit')
from mot import Mot
from common import motion
D=np.radians
def Rx(a): c,s=np.cos(a),np.sin(a); return np.array([[1,0,0],[0,c,-s],[0,s,c]])
def Ry(a): c,s=np.cos(a),np.sin(a); return np.array([[c,0,s],[0,1,0],[-s,0,c]])
def Rz(a): c,s=np.cos(a),np.sin(a); return np.array([[c,-s,0],[s,c,0],[0,0,1]])
R=lambda e: Rz(e[2])@Ry(e[1])@Rx(e[0])
body=sys.argv[1]; punish=sys.argv[2] if len(sys.argv)>2 else 'Spanking'
lines=open(ROOT+'/resources/punish_fit.txt').read().split('\n')
i=next(k for k,l in enumerate(lines) if l.startswith('fit %s '%punish) and body in l.split()[2].split(','))
tracks={}
for l in lines[i+1:]:
    if l.startswith('fit ') : break
    if l.startswith('gene '):
        p=l.split(); keys=[]
        for it in p[2:]:
            f,v=it.split(':'); vals=[float(x) for x in v.split(',')]; keys.append((float(f),vals+[0.0]*(13-len(vals))))
        tracks[int(p[1])]=keys
def at(keys,f):
    if f<=keys[0][0] or len(keys)==1: return np.array(keys[0][1])
    if f>=keys[-1][0]: return np.array(keys[-1][1])
    for (f0,a),(f1,b) in zip(keys,keys[1:]):
        if f0<=f<=f1: t=(f-f0)/(f1-f0); return np.array(a)+(np.array(b)-np.array(a))*t
for mi,keys in tracks.items():
    t,blob=motion(mi); open('src.bin','wb').write(blob); a=Mot(blob)
    V=np.array([at(keys,f) for f in range(a.frame_count)])
    np.savetxt('vals.txt',V); subprocess.run([__import__('os').environ.get('H3',os.path.join(os.path.dirname(os.path.abspath(__file__)),'apply_fit')),'src.bin','out.bin','vals.txt'],check=True)
    b=Mot(open('out.bin','rb').read())
    def target(bone,ff):
        v=at(keys,ff); x=D(v[4:12]); tw=D(v[12])
        pre={1:Rz(x[1])@Rx(x[0]),13:Rz(x[2])@Ry(x[3]),12:Rz(x[2]/2)@Ry(x[3]/2),14:Ry(x[4]),15:np.eye(3),16:np.eye(3),7:Rz(-x[5])@Ry(-x[6]),8:Ry(-x[7])}[bone]
        post=Rx(tw/2) if bone in (15,16) else np.eye(3)
        return pre@R(a.pose(ff,30)[bone][1])@post
    rep=[]
    for bone in (1,7,8,13,14,16):
        errs=[]
        for ff in np.arange(0,a.frame_count-1,0.5):
            d=R(b.pose(ff,30)[bone][1]).T@target(bone,ff); errs.append(np.degrees(np.arccos(np.clip((np.trace(d)-1)/2,-1,1))))
        errs=np.array(errs); k=np.argmax(errs); rep.append('b%d max %.1f@%.1f'%(bone,errs[k],k*0.5))
    print('motion',mi,'frames',a.frame_count,' '.join(rep))
if len(sys.argv)>3:
    mi=int(sys.argv[3]); bone=int(sys.argv[4]); keys=tracks[mi]
    t,blob=motion(mi); a=Mot(blob); V=np.array([at(keys,f) for f in range(a.frame_count)])
    np.savetxt('vals.txt',V); open('src.bin','wb').write(blob); subprocess.run([__import__('os').environ.get('H3',os.path.join(os.path.dirname(os.path.abspath(__file__)),'apply_fit')),'src.bin','out.bin','vals.txt'],check=True)
    b=Mot(open('out.bin','rb').read())
    for ff in np.arange(float(sys.argv[5]),float(sys.argv[6]),0.5):
        print('%5.1f own'%ff,np.round(np.degrees(a.pose(ff,30)[bone][1]),1),' new',np.round(np.degrees(b.pose(ff,30)[bone][1]),1))
