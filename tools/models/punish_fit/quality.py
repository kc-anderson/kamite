import numpy as np
from scipy.spatial import cKDTree
from sim import *
from contacts import *
def vnormals(V,T):
    n=np.zeros_like(V); a=V[T[:,0]];b=V[T[:,1]];c=V[T[:,2]]; fn=np.cross(b-a,c-a)
    for k in range(3): np.add.at(n,T[:,k],fn)
    n/=np.linalg.norm(n,axis=1,keepdims=True)+1e-12; return n
def assess(name,path,scale=1.0,fits=None,every=2):
    fr=simulate(name,path,scale,fits=fits,keep_mesh=True,every=every)
    g=actor(GENE_PATH); v=actor(path,scale)
    gd=dominant(g.mesh); vd=dominant(v.mesh)
    pen=[];touch=0;pairs=set()
    for f in fr:
        tree=cKDTree(f['vV']); d,i=tree.query(f['gV'],distance_upper_bound=0.15)
        ok=np.isfinite(d)
        if not ok.any(): pen.append(0); continue
        nv=vnormals(f['vV'],f['vT'])
        gi=np.nonzero(ok)[0]; vi=i[gi]
        s=np.einsum('ij,ij->i',f['gV'][gi]-f['vV'][vi],nv[vi])
        inside=s<-0.01
        pen.append(float(d[gi][inside].max()) if inside.any() else 0.0)
        close=d[gi]<0.03
        touch+=int(close.sum()>0)
        for k in gi[close]:
            gp=GENE_PART.get(int(gd[k])); vp=VIC_PART.get(int(vd[i[k]]))
            if gp and vp: pairs.add((gp,vp))
    pen=np.array(pen)
    return dict(frames=len(fr),touch=touch,pairs=len(pairs),pen_mean=pen.mean(),pen_p90=np.percentile(pen,90),pen_max=pen.max())
