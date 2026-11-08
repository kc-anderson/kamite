import numpy as np, math
from scene import *
from punishes import PUNISH
from raster import render
from PIL import Image

GENE_PATH=EX+'/pl/pl00.dat'
_cache={}
def actor(path,scale=1.0):
    a=_cache.get(path)
    if a is None: a=_cache[path]=Actor(path)
    a.scale=scale; a.pos=np.zeros(3); a.yaw=0.0
    return a

def simulate(name, victim_path, vscale=1.0, fits=None, keep_mesh=False, every=1):
    """fits: per stage index -> fit dict. Returns frames: dict(stage, t, gP, vP, gV?, vV?)."""
    gene=actor(GENE_PATH); vic=actor(victim_path,vscale)
    gene.pos=np.array([0,0,1.2]); gene.yaw=math.pi   # in front of the enemy, facing it
    vic.pos=np.zeros(3); vic.yaw=0.0
    gt=Track(gene); vt=Track(vic)
    out=[]
    for si,(gm,vm,al) in enumerate(PUNISH[name]):
        if al:
            vec=(al[0],al[1]); fit=dict((fits or {}).get(si,{}))
            if len(al)>4 and al[4]=='unscaled': fit['unscaled']=True
            align(gene,vic,vec,al[2],al[3],fit)
        gt.start(gm); vt.start(vm)
        total=sum(Motion(m).frames for m in gm)
        for k in range(total):
            if k>0: gt.advance(); vt.advance()
            if k%every: continue
            Wg,Pg=gt.pose(); Wv,Pv=vt.pose()
            rec=dict(stage=si,t=k,gm=gt.cur()[0].idx,vm=vt.cur()[0].idx)
            rec['gP']=np.array([gene.world(p) for p in Pg]); rec['vP']=np.array([vic.world(p) for p in Pv])
            if keep_mesh:
                rec['gV']=gene.world(skin(gene.mesh,Wg,Pg)); rec['vV']=vic.world(skin(vic.mesh,Wv,Pv))
                rec['gT']=gene.mesh['T']; rec['vT']=vic.mesh['T']
            out.append(rec)
    return out

def views():
    side=np.array([[0,0,-1],[0,1,0],[1,0,0]],float)
    a=math.radians(35)
    three=np.array([[math.cos(a),0,-math.sin(a)],[0,1,0],[math.sin(a),0,math.cos(a)]])@np.array([[0,0,-1],[0,1,0],[1,0,0]],float)
    front=np.array([[1,0,0],[0,1,0],[0,0,1]],float)
    out=np.array([0.62,0.32,0.72]); out/=np.linalg.norm(out)
    up=np.array([0,1.0,0])-out*out[1]; up/=np.linalg.norm(up); right=np.cross(up,out)
    three=np.stack([right,up,out])
    return dict(side=side,front=front,three=three)

def strip(frames, view, path, marks_fn=None, size=(360,300), label=True):
    from PIL import ImageDraw
    allv=np.concatenate([np.concatenate([f['gV'],f['vV']]) for f in frames])@view.T
    lo=allv[:,:2].min(0); hi=allv[:,:2].max(0); c=(lo+hi)/2; span=(hi-lo).max()*1.05
    imgs=[]
    for f in frames:
        marks=marks_fn(f) if marks_fn else ()
        im,_=render([(f['gV'],f['gT'],(90,120,200)),(f['vV'],f['vT'],(220,140,80))],view,size=size,extent=(c,span),marks=marks)
        pim=Image.fromarray(im)
        if label:
            ImageDraw.Draw(pim).text((6,4),'st%d f%d (%d/%d)'%(f['stage'],f['t'],f['gm'],f['vm']),fill=(0,0,0))
        imgs.append(pim)
    W=sum(i.width for i in imgs); H=imgs[0].height
    sheet=Image.new('RGB',(W,H),(255,255,255)); x=0
    for i in imgs: sheet.paste(i,(x,0)); x+=i.width
    sheet.save(path); return path
