import sys, numpy as np
from fit3 import *
from fit import reference
from raster import render
from PIL import Image, ImageDraw
def sheet(name, body, picks, out, view='side', size=(300,280)):
    fr_ref,tch=reference(name)
    from report1 import REF
    refP=prepare(name,tch,EX+'/em/%s.dat'%REF[name][0])
    P=prepare(name,tch,EX+'/em/%s.dat'%body)
    rp=solve(P,use=(1,1,1,0,0,0)); ra=solve(P)
    rows=[('original: '+REF[name][0],reposed(refP,None,EX+'/em/%s.dat'%REF[name][0])),
          (body+': no fit',reposed(P,None,EX+'/em/%s.dat'%body)),
          (body+': placement only (%.0f cm off)'%(rp['e1'].mean()*100),reposed(P,rp['X'],EX+'/em/%s.dat'%body)),
          (body+': placement + heights + bend (%.0f cm off)'%(ra['e1'].mean()*100),reposed(P,ra['X'],EX+'/em/%s.dat'%body))]
    V=views()[view]
    sel=[]
    for lab,frs in rows:
        idx={(f['stage'],f['t']):f for f in frs}
        sel.append((lab,[idx[p] for p in picks]))
    allv=np.concatenate([np.concatenate([f['gV'],f['vV']]) for lab,fs in sel for f in fs])@V.T
    lo=allv[:,:2].min(0); hi=allv[:,:2].max(0)
    W,H=size
    img=Image.new('RGB',(W*len(picks),(H+18)*len(rows)),(255,255,255)); d=ImageDraw.Draw(img)
    for ri,(lab,fs) in enumerate(sel):
        # per column extent shared across rows
        for ci,f in enumerate(fs):
            col=np.concatenate([np.concatenate([s[1][ci]['gV'],s[1][ci]['vV']]) for s in sel])@V.T
            lo=col[:,:2].min(0); hi=col[:,:2].max(0); c=(lo+hi)/2; span=(hi-lo).max()*1.08
            im,_=render([(f['gV'],f['gT'],(90,120,200)),(f['vV'],f['vT'],(220,140,80))],V,size=(H,W),extent=(c,span))
            img.paste(Image.fromarray(im),(ci*W,ri*(H+18)+18))
            if ri==0: d.text((ci*W+4,ri*(H+18)+20),'%s f%d'%(PUNISH[name][f['stage']]['label'],f['t']),fill=(0,0,0))
        d.text((4,ri*(H+18)+3),lab,fill=(0,0,0))
    img.save(out); return ra,rp
if __name__=='__main__':
    pass
