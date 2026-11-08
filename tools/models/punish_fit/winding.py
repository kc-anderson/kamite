import numpy as np
def winding(points, V, T):
    """Generalized winding number of each point w.r.t. the triangle soup (V, T): ~1 inside a closed surface, ~0 out."""
    a=V[T[:,0]][None]-points[:,None]; b=V[T[:,1]][None]-points[:,None]; c=V[T[:,2]][None]-points[:,None]
    la=np.linalg.norm(a,axis=2); lb=np.linalg.norm(b,axis=2); lc=np.linalg.norm(c,axis=2)
    det=np.einsum('ijk,ijk->ij',a,np.cross(b,c))
    den=la*lb*lc+np.einsum('ijk,ijk->ij',a,b)*lc+np.einsum('ijk,ijk->ij',b,c)*la+np.einsum('ijk,ijk->ij',c,a)*lb
    return np.arctan2(det,den).sum(1)/(2*np.pi)
def near_tris(V,T,points,margin=0.35):
    lo=points.min(0)-margin; hi=points.max(0)+margin
    c=V[T].mean(1); m=np.all((c>=lo)&(c<=hi),axis=1); return T[m]
