import sys, json, os, numpy as np
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from fit3 import prepare, NAMES, offset_at, NP
from fit import reference
from sim import *
from raster import render
from PIL import Image, ImageDraw
from fk import Motion, pose
OUT=os.environ.get('PUNISH_FIT_REVIEW','site2'); os.makedirs(OUT,exist_ok=True)
PICKS={'Stinger':[(0,7),(1,2),(1,11),(2,0),(2,20),(2,34)],
       'Cobra':[(0,10),(0,24),(1,6),(2,10),(2,30),(2,40)],
       'Spanking':[(0,8),(0,20),(1,4),(1,12),(2,10),(2,21)],
       'Suplex':[(0,8),(0,16),(0,24),(0,30),(0,34),(0,38)]}
REF={'Suplex':'em00','Stinger':'em18','Cobra':'em07','Spanking':'em16'}
_m={}
def M(i):
    if i not in _m: _m[i]=Motion(i)
    return _m[i]
def repose(f, X, P, path):
    g=actor(GENE_PATH); v=actor(path)
    o=offset_at(P,X,f['stage'],f['t']) if X is not None else np.zeros(NP)
    Wg,Pg=pose(g.skel,M(f['gm']),f['gt'],hip_dy=o[3]); Wv,Pv=pose(v.skel,M(f['vm']),f['vt'],hip_dy=o[4],bend=o[5])
    w=lambda a,pos,yaw,pts: pos+(roty(yaw)@(a.scale*pts.T)).T
    from fit3 import apply_gene_rows
    gV=apply_gene_rows(w(g,f['gpos'],f['gyaw'],skin(g.mesh,Wg,Pg)),f['gpos'],f['gyaw'],o)
    vV=w(v,f['vpos'],f['vyaw'],skin(v.mesh,Wv,Pv))
    return dict(gV=gV,vV=vV,gT=g.mesh['T'],vT=v.mesh['T'],stage=f['stage'],t=f['t'])
def card(name, group, rep, tag):
    fr_ref,tch=reference(name)
    refpath=EX+'/em/%s.dat'%REF[name]; path=EX+'/em/%s.dat'%rep
    refP=prepare(name,tch,refpath); P=prepare(name,tch,path)
    X=np.load('fits/%s_%02d.npy'%(name,group))
    rows=[('original (%s)'%REF[name],refP,None,refpath),('%s, no fit'%rep,P,None,path),('%s, fitted'%rep,P,X,path)]
    V=views()['side']; W,H=260,250
    picks=PICKS[name]
    cells=[]
    for lab,PP,XX,pp in rows:
        idx={(f['stage'],f['t']):f for f in PP['fr']}
        cells.append([repose(idx[p],XX,PP,pp) for p in picks])
    img=Image.new('RGB',(W*len(picks),(H+16)*len(rows)),(255,255,255)); d=ImageDraw.Draw(img)
    for ci in range(len(picks)):
        col=np.concatenate([np.concatenate([cells[r][ci]['gV'],cells[r][ci]['vV']]) for r in range(len(rows))])@V.T
        lo=col[:,:2].min(0); hi=col[:,:2].max(0); c=(lo+hi)/2; span=(hi-lo).max()*1.06
        for ri in range(len(rows)):
            f=cells[ri][ci]
            im,_=render([(f['gV'],f['gT'],(90,120,200)),(f['vV'],f['vT'],(220,140,80))],V,size=(H,W),extent=(c,span))
            img.paste(Image.fromarray(im),(ci*W,ri*(H+16)+16))
            if ri==0: d.text((ci*W+4,18),'%s f%d'%(PUNISH[name][f['stage']]['label'],f['t']),fill=(0,0,0))
    for ri,(lab,_,_,_) in enumerate(rows): d.text((4,ri*(H+16)+2),lab,fill=(0,0,0))
    sheet='%s/%s.png'%(OUT,tag); img.save(sheet)
    # GIF of the fitted pair, 3/4 view, every 2nd frame
    V3=views()['three']; frs=P['fr'][::2]
    reps=[repose(f,X,P,path) for f in frs]
    allv=np.concatenate([np.concatenate([r['gV'],r['vV']]) for r in reps[::4]])@V3.T
    lo=allv[:,:2].min(0); hi=allv[:,:2].max(0); c=(lo+hi)/2; span=(hi-lo).max()*1.1
    ims=[]
    for r in reps:
        im,_=render([(r['gV'],r['gT'],(90,120,200)),(r['vV'],r['vT'],(220,140,80))],V3,size=(260,260),extent=(c,span))
        p=Image.fromarray(im); ImageDraw.Draw(p).text((4,4),'%s f%d'%(PUNISH[name][r['stage']]['label'],r['t']),fill=(0,0,0))
        ims.append(p.convert('P',palette=Image.ADAPTIVE,colors=48))
    gif='%s/%s.gif'%(OUT,tag)
    ims[0].save(gif,save_all=True,append_images=ims[1:],duration=66,loop=0,optimize=True)
    return sheet,gif
if __name__=='__main__':
    name,group,rep,tag=sys.argv[1],int(sys.argv[2]),sys.argv[3],sys.argv[4]
    print(card(name,group,rep,tag))
