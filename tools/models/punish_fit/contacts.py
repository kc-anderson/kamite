import numpy as np
from scipy.spatial import cKDTree
GENE_PART={9:'R hand',10:'R hand',15:'L hand',16:'L hand',8:'R forearm',14:'L forearm',7:'R upper arm',13:'L upper arm',6:'R shoulder',12:'L shoulder',
           19:'R shin/knee',23:'L shin/knee',18:'R thigh',22:'L thigh',20:'R foot',21:'R foot',24:'L foot',25:'L foot',4:'head',3:'neck',2:'chest',1:'belly',0:'hips',17:'hips',26:'head'}
VIC_PART={4:'head',26:'head',29:'head',30:'head',3:'neck',2:'chest',1:'belly',0:'hips',17:'hips',5:'R shoulder',11:'L shoulder',6:'R shoulder',12:'L shoulder',
          7:'R upper arm',13:'L upper arm',8:'R forearm',14:'L forearm',9:'R hand',10:'R hand',15:'L hand',16:'L hand',
          18:'R thigh',22:'L thigh',19:'R shin',23:'L shin',20:'R foot',21:'R foot',24:'L foot',25:'L foot'}
def dominant(mesh):
    J=mesh['J']; W=mesh['W']; return J[np.arange(len(J)),W.argmax(1)]
def frame_contacts(f, gdom, vdom, thr=0.03):
    tree=cKDTree(f['vV'])
    d,i=tree.query(f['gV'],distance_upper_bound=0.2)
    out={}
    ok=np.isfinite(d)
    for gi in np.nonzero(ok)[0]:
        gp=GENE_PART.get(int(gdom[gi])); vp=VIC_PART.get(int(vdom[i[gi]]))
        if not gp or not vp: continue
        k=(gp,vp)
        if d[gi]<out.get(k,(9,))[0]: out[k]=(d[gi],gi,int(i[gi]))
    return out
