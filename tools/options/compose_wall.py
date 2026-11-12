# The OPTIONS screen's wall (resources/options/wall.jpg) from K's 16:9 wall and its blank version: 32:9, his wall in
# the middle untouched, widened on both sides with stretches of the blank wall (its generic left and right parts, so
# the scratched middle doesn't repeat), each joined with a 140 px crossfade where the brick rows line up best, and
# darkened gently past the 21:9 area. The game crops it to the picture's shape (src/kamite/options_screen.cpp).
#   python3 compose_wall.py kamite_wall.jpg kamite_wall_blank.jpg wall.jpg
from PIL import Image; import numpy as np, sys
G=np.asarray(Image.open(sys.argv[1]).convert('RGB'),dtype=np.float32)
B=np.asarray(Image.open(sys.argv[2]).convert('RGB'),dtype=np.float32)
H,W,_=G.shape
k=np.ones(31)/31
def hp(img):
    L=img.mean(axis=2); return L-np.apply_along_axis(lambda c: np.convolve(c,k,'same'),1,L)
HPB=hp(B)
rows=slice(150,1500)
o=140
e=(int(round(H*32/9))-W)//2+2
Wt=W+2*e
out=np.zeros((H,Wt,3),np.float32); out[:,e:e+W]=G
cur=np.zeros((H,Wt),np.float32); cur[:,e:e+W]=hp(G)
def corr(a,b):
    a=(a-a.mean())/(a.std()+1e-6); b=(b-b.mean())/(b.std()+1e-6); return float((a*b).mean())
def place(x0,x1,nominal,seam_right):
    seam=np.arange(x1-o,x1) if seam_right else np.arange(x0,x0+o)
    best=None
    for sh in range(nominal-60,nominal+61):
        c=np.arange(x0,x1)-sh
        if c.min()<0 or c.max()>=W: continue
        v=corr(cur[rows][:,seam],HPB[rows][:,seam-sh])
        if best is None or v>best[1]: best=(sh,v)
    sh=best[0]; print('seg',x0-e,x1-e,'B',x0-sh,x1-1-sh,'corr',round(best[1],3))
    for x in range(x0,x1):
        c=B[:,x-sh]
        if seam_right and x>=x1-o: t=(x-(x1-o))/o; t=t*t*(3-2*t); out[:,x]=c*(1-t)+out[:,x]*t
        elif (not seam_right) and x<x0+o: t=(x-x0)/o; t=t*t*(3-2*t); out[:,x]=out[:,x]*(1-t)+c*t
        else: out[:,x]=c; cur[:,x]=HPB[:,x-sh]
s1=e-820
place(s1,e+o,(e+o-1)-(W-40),True)
place(0,s1+o,-14,True)
r1=e+W+820
place(e+W-o,r1,(e+W-o)-40,False)
place(r1-o,Wt,(Wt-1)-(W-14),False)
w21=H*21/9
xs=np.arange(Wt)-Wt/2; d=np.clip((np.abs(xs)-w21/2)/(Wt/2-w21/2),0,1)
out*=(1-0.4*d**1.5)[None,:,None]
img=Image.fromarray(np.clip(out+0.5,0,255).astype(np.uint8))
img.save(sys.argv[3],quality=92,subsampling=0)
print(img.size)
