import os, numpy as np
ROOT=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),'../../../..'))
def load(body,punish):
    lines=open(ROOT+'/resources/punish_fit.txt').read().split('\n')
    i=next(k for k,l in enumerate(lines) if l.startswith('fit %s '%punish) and body in l.split()[2].split(','))
    tracks={}
    for l in lines[i+1:]:
        if l.startswith('fit '): break
        if l.startswith('gene '):
            p=l.split(); keys=[]
            for it in p[2:]:
                f,v=it.split(':'); vals=[float(x) for x in v.split(',')]; keys.append((float(f),vals+[0.0]*(13-len(vals))))
            tracks[int(p[1])]=keys
    return tracks
def at(keys,f):
    if f<=keys[0][0] or len(keys)==1: return np.array(keys[0][1])
    if f>=keys[-1][0]: return np.array(keys[-1][1])
    for (f0,a),(f1,b) in zip(keys,keys[1:]):
        if f0<=f<=f1: t=(f-f0)/(f1-f0); return np.array(a)+(np.array(b)-np.array(a))*t
