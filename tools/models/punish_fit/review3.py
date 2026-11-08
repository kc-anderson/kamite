"""Review cards for fit7 (step 3: Gene's pose). Per punish and body type, at enemy size 1.0: the original pairing, the
current build (fit3: placement and heights only) and the new fit; at 1.3: the original at its own size, the scaled body
with no fit, and the new fit. Side view, Gene blue, the victim orange."""
import sys, os, json, numpy as np
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
import fit7
from fit7 import *
from fit_pose import REF, REFTYPE, REP, REPFILE, OUT as FITS, touches_for, flat_for
from raster import render
from PIL import Image, ImageDraw
SITE=os.environ.get('PUNISH_FIT_SITE3',os.path.join(HERE,'site3')); os.makedirs(SITE,exist_ok=True)
PICKS={'Stinger':[(0,7),(1,2),(2,0),(2,20)],'Cobra':[(0,10),(1,6),(2,10),(2,30)],
       'Spanking':[(0,8),(0,20),(1,4),(1,12)],'Suplex':[(0,8),(0,16),(0,24),(0,34)],'Gorilla':[(0,10),(0,20),(0,30),(0,42)]}
GT=actor(GENE_PATH).mesh['T']
def rows_at(P,X,sz,picks):
    out=[]
    for (si,t) in picks:
        i=P['idx'][(si,t)]; f=P['fr'][i]
        if X is None: O=np.zeros(NP); s_=1.0
        else:
            k0,k1,a=P['bracket'](si,t); O=X[k0]*(1-a)+X[k1]*a; s_=sz
        n=len(P['gd']); d=gene_frame_data(P['fr'],P['gd'],np.full(n,i),np.arange(n))
        out.append(dict(gV=gene_xform(d,np.repeat(O[None],n,0)),vV=victim_full(P,f,O,s_),vT=P['vT'],label='%s f%d'%(PUNISH[P['name']][si]['label'],t)))
    return out
def fit3_rows(name,group,path,picks):
    """The current build's fit (fit3) for the body, posed by review2's repose."""
    import review2, fit3
    from fit import reference
    fp=os.path.join(HERE,'fits','%s_%02d.npy'%(name,group))
    if not os.path.exists(fp): return None
    fr_ref,tch=reference(name); P=fit3.prepare(name,tch,path); X=np.load(fp)
    idx={(f['stage'],f['t']):f for f in P['fr']}; out=[]
    for p in picks:
        r=review2.repose(idx[p],X,P,path); out.append(dict(gV=r['gV'],vV=r['vV'],vT=r['vT'],label=''))
    return out
def sheet(rows,labels,path,W=230,H=230):
    V=views()['side']; nc=len(rows[0])
    img=Image.new('RGB',(W*nc,(H+16)*len(rows)),(255,255,255)); d=ImageDraw.Draw(img)
    for ci in range(nc):
        col=np.concatenate([np.concatenate([r[ci]['gV'],r[ci]['vV']]) for r in rows])@V.T
        lo=col[:,:2].min(0); hi=col[:,:2].max(0); c=(lo+hi)/2; span=(hi-lo).max()*1.06
        for ri,r in enumerate(rows):
            im,_=render([(r[ci]['gV'],GT,(90,120,200)),(r[ci]['vV'],r[ci]['vT'],(220,140,80))],V,size=(H,W),extent=(c,span))
            img.paste(Image.fromarray(im),(ci*W,ri*(H+16)+16))
        d.text((ci*W+4,18),rows[-1][ci]['label'],fill=(0,0,0))
    for ri,lab in enumerate(labels): d.text((4,ri*(H+16)+2),lab,fill=(0,0,0))
    img.save(path)
def gif(P,X,sz,path,every=2):
    V3=views()['three']; frs=[(f['stage'],f['t']) for f in P['fr'] if f['touch']][::every]
    rs=rows_at(P,X,sz,frs)
    allv=np.concatenate([np.concatenate([r['gV'],r['vV']]) for r in rs[::4]])@V3.T
    lo=allv[:,:2].min(0); hi=allv[:,:2].max(0); c=(lo+hi)/2; span=(hi-lo).max()*1.1; ims=[]
    for r in rs:
        im,_=render([(r['gV'],GT,(90,120,200)),(r['vV'],r['vT'],(220,140,80))],V3,size=(260,260),extent=(c,span))
        p=Image.fromarray(im); ImageDraw.Draw(p).text((4,4),r['label'],fill=(0,0,0)); ims.append(p.convert('P',palette=Image.ADAPTIVE,colors=48))
    ims[0].save(path,save_all=True,append_images=ims[1:],duration=66,loop=0,optimize=True)
def card(name,t,s,make_gif=False):
    tag='%s_%s_%.2f'%(name,t,s); jp=os.path.join(FITS,'rep_%s.json'%tag)
    if not os.path.exists(jp): return None
    m=json.load(open(jp)); X=np.load(jp[:-5]+'.npy'); sz=m['size']
    tch,base=touches_for(name,t); refpath=os.path.join(EX,'em','%s.dat'%REF[name]); path=os.path.join(EX,'em','%s.dat'%REPFILE[t])
    picks=PICKS[name]
    Pr=prepare(name,tch,base,refpath,1.0); P=prepare(name,tch,base,path,s)
    rows=[rows_at(Pr,None,1.0,picks)]; labels=['original (%s)'%REF[name]]
    if s==1.0:
        r3=fit3_rows(name,REP[t],path,picks)
        if r3: rows.append(r3); labels.append('%s, current build (placement and heights)'%REPFILE[t])
    else:
        rows.append(rows_at(P,None,1.0,picks)); labels.append('%s at size %.1f, no fit'%(REPFILE[t],s))
    rows.append(rows_at(P,X,sz,picks)); labels.append('%s%s, new fit (with Gene\'s pose)'%(REPFILE[t],'' if s==1.0 else ' at size %.1f'%s))
    sheet(rows,labels,os.path.join(SITE,tag+'.png'))
    if make_gif: gif(P,X,sz,os.path.join(SITE,tag+'.gif'))
    return tag
if __name__=='__main__':
    from multiprocessing import Pool
    gifs={('Gorilla','fat',1.0),('Cobra','women',1.0),('Cobra','fat',1.0),('Spanking','men',1.0),('Stinger','men',1.0)}
    jobs=[]
    for f in sorted(os.listdir(FITS)):
        if f.startswith('rep_') and f.endswith('.json'):
            _,name,t,s=f[:-5].split('_'); s=float(s); jobs.append((name,t,s,(name,t,s) in gifs))
    with Pool(2) as p:
        for r in p.starmap(card,jobs): print(r,flush=True)
