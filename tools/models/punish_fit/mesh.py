import numpy as np
from common import *

def keep_node(name):
    n=name
    if 'mip' in n or n.endswith('_mi') or n.endswith('_mip'): return False
    for p in ('RH_','LH_','HD_'):
        if n.startswith(p) and not n.startswith(p+'00'): return False
    if n.startswith('z_') or n.startswith('y_cloth') or n=='hat' or n.startswith('HD_00_mi'): return False
    return True

def load_mesh(skel):
    """Vertices in rest space, per-vertex 3 joints/weights, triangles."""
    raw=skel['raw']; V=[];J=[];Wt=[];T=[]
    for nd in skel['nodes']:
        if not keep_node(nd['name']): continue
        m=mdb_read(raw,nd['mdb_off'])
        for vb in m['vbs']:
            if not vb['wt']: continue
            base=len(V)
            for k in range(vb['count']):
                V.append(vb['pos'][k]); jb,wb=vb['wt'][k]
                J.append([jb[1]//4,jb[2]//4,jb[3]//4]); w=np.array([wb[0],wb[1],wb[2]],float); Wt.append(w/(w.sum() or 1))
            for a,b,c in strip_tris(vb['flags']): T.append((base+a,base+b,base+c))
    bones=skel['bones']; rest=[]
    for i,b in enumerate(bones):
        p=b['parent']; rest.append(np.array(b['t'],float)+(rest[p] if 0<=p<i else 0))
    return dict(V=np.array(V),J=np.array(J),W=np.array(Wt),T=np.array(T),rest=np.array(rest))

def skin(mesh, Wm, Pm):
    """Model-space skinned vertices from bone world matrices/positions."""
    V=mesh['V']; out=np.zeros_like(V)
    n=len(Wm)
    R=np.stack([Wm[j] if Wm[j] is not None else np.eye(3) for j in range(n)])
    P=np.stack([Pm[j] if Pm[j] is not None else np.zeros(3) for j in range(n)])
    rest=mesh['rest']
    for k in range(3):
        j=np.clip(mesh['J'][:,k],0,n-1)
        loc=V-rest[j]
        out+=mesh['W'][:,k:k+1]*(np.einsum('nij,nj->ni',R[j],loc)+P[j])
    return out
