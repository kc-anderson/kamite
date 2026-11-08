import sys, json, os
import numpy as np
from sim import *
from contacts import *
from PIL import Image, ImageDraw
REF={'Suplex':('em00','a thug (em00, the standard man\'s skeleton)'),
     'Stinger':('em18','a fat man (em18, the fat men\'s skeleton)'),
     'Cobra':('em07','a big man (em07, the tall men\'s skeleton)'),
     'Spanking':('em16','a woman (em16, the women\'s skeleton)')}
from punishes import PUNISH
STAGES={k:[s['label'] for s in v] for k,v in PUNISH.items()}
MAIN_G={'R hand','L hand','R forearm','L forearm','R upper arm','L upper arm','R shin/knee','L shin/knee','R thigh','L thigh','R foot','L foot','chest','belly','hips','head'}
LIMB={'R hand','L hand','R forearm','L forearm','R upper arm','L upper arm','R shin/knee','L shin/knee','R thigh','L thigh','R foot','L foot'}
OUT=os.environ.get('PUNISH_FIT_OUT','out1'); os.makedirs(OUT,exist_ok=True)
def run(name):
    body,desc=REF[name]; path=EX+'/em/%s.dat'%body
    fr=simulate(name,path,1.0,keep_mesh=True)
    g=actor(GENE_PATH); v=actor(path)
    gd=dominant(g.mesh); vd=dominant(v.mesh)
    per=[]; ev={}
    for k,f in enumerate(fr):
        c={} if not f['touch'] else {key:val for key,val in frame_contacts(f,gd,vd).items() if val[0]<(0.05 if 'knee' in key[0] or 'foot' in key[0] else 0.03) and key[0] in MAIN_G}
        per.append(c)
        for key in c: ev.setdefault((f['stage'],key),[]).append(f['t'])
    # touch list per stage
    touches=[]
    for (st,key),ts in sorted(ev.items(),key=lambda kv:(kv[0][0],-len(kv[1]))):
        if len(ts)<3: continue
        touches.append(dict(stage=st,gene=key[0],victim=key[1],first=min(ts),last=max(ts),frames=len(ts)))
    # key frames: per stage, the frame with most main contacts (and the stage's first frame)
    keys=[]
    for st in range(len(STAGES[name])):
        idx=[k for k,f in enumerate(fr) if f['stage']==st]
        if not idx: continue
        best=max(idx,key=lambda k:(len(per[k]),-k))
        cand=[idx[0],best]
        # also the stage's last contact frame if far from best
        withc=[k for k in idx if per[k]]
        if withc and withc[-1]-best>8: cand.append(withc[-1])
        if not withc: cand.append(idx[len(idx)//2])
        for k in sorted(set(cand)): keys.append(k)
    keys=sorted(set(keys))
    V=views()
    def marks(k):
        f=fr[k]; out=[]
        items=sorted([(d,key,gi,vi) for key,(d,gi,vi) in per[k].items() if key[0] in LIMB])[:6]
        for d,key,gi,vi in items:
            p=(f['gV'][gi]+f['vV'][vi])/2; out.append((p,(230,20,20),5))
        return out
    sheets=[]
    for vn in ('three','side'):
        sel=[fr[k] for k in keys]
        allv=np.concatenate([np.concatenate([f['gV'],f['vV']]) for f in sel])@V[vn].T
        lo=allv[:,:2].min(0); hi=allv[:,:2].max(0); c=(lo+hi)/2; span=(hi-lo).max()*1.05
        imgs=[]
        for k in keys:
            f=fr[k]
            im,_=render([(f['gV'],f['gT'],(90,120,200)),(f['vV'],f['vT'],(220,140,80))],V[vn],size=(340,300),extent=(c,span),marks=marks(k))
            pim=Image.fromarray(im); dr=ImageDraw.Draw(pim)
            dr.text((6,4),'%s, frame %d'%(STAGES[name][f['stage']],f['t']),fill=(0,0,0))
            imgs.append(pim)
        cols=min(len(imgs),4); rows=(len(imgs)+cols-1)//cols
        sheet=Image.new('RGB',(cols*340,rows*300),(255,255,255))
        for i,im in enumerate(imgs): sheet.paste(im,((i%cols)*340,(i//cols)*300))
        p='%s/%s_%s.png'%(OUT,name.lower(),vn); sheet.save(p); sheets.append(p)
    # animation (3/4 view), every frame
    allv=np.concatenate([np.concatenate([f['gV'],f['vV']]) for f in fr[::5]])@V['three'].T
    lo=allv[:,:2].min(0); hi=allv[:,:2].max(0); c=(lo+hi)/2; span=(hi-lo).max()*1.1
    frames=[]
    for k,f in enumerate(fr):
        im,_=render([(f['gV'],f['gT'],(90,120,200)),(f['vV'],f['vT'],(220,140,80))],V['three'],size=(320,320),extent=(c,span),marks=marks(k))
        pim=Image.fromarray(im); ImageDraw.Draw(pim).text((6,4),'%s f%d'%(STAGES[name][f['stage']],f['t']),fill=(0,0,0))
        frames.append(pim.convert('P',palette=Image.ADAPTIVE,colors=64))
    gif='%s/%s.gif'%(OUT,name.lower())
    frames[0].save(gif,save_all=True,append_images=frames[1:],duration=33,loop=0,optimize=True)
    json.dump(dict(name=name,body=body,desc=desc,stages=STAGES[name],touches=touches,keys=[(fr[k]['stage'],fr[k]['t']) for k in keys],
                   lengths=[sum(1 for f in fr if f['stage']==s) for s in range(len(STAGES[name]))]),open('%s/%s.json'%(OUT,name.lower()),'w'),indent=1)
    return touches
if __name__=='__main__':
    for n in sys.argv[1:]:
        t=run(n); print(n,len(t),'touches')
