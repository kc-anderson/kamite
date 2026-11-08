import numpy as np
def render(objs, view, size=(520,520), extent=None, bg=(250,250,250), marks=()):
    """objs: list of (verts_world Nx3, tris Mx3, rgb). view: 3x3 rotation world->camera (x right, y up, z toward viewer).
    Orthographic. Returns HxWx3 uint8 and the projection used."""
    H,Wd=size
    allv=np.concatenate([o[0] for o in objs])@view.T
    if extent is None:
        lo=allv[:,:2].min(0); hi=allv[:,:2].max(0); c=(lo+hi)/2; span=(hi-lo).max()*1.1
    else:
        c,span=extent
    scale=min(H,Wd)/span
    img=np.zeros((H,Wd,3),float); img[:]=bg
    zb=np.full((H,Wd),-1e9)
    light=np.array([0.3,0.6,0.75]); light/=np.linalg.norm(light)
    def proj(v):
        cv=v@view.T
        x=(cv[:,0]-c[0])*scale+Wd/2; y=H/2-(cv[:,1]-c[1])*scale
        return np.stack([x,y,cv[:,2]],1)
    for V,T,col in objs:
        pv=proj(V); cv=V@view.T
        a=cv[T[:,0]];b=cv[T[:,1]];cc=cv[T[:,2]]
        nrm=np.cross(b-a,cc-a); ln=np.linalg.norm(nrm,axis=1)+1e-12; nrm/=ln[:,None]
        shade=np.abs(nrm@light)*0.75+0.25
        for t in range(len(T)):
            p=pv[T[t]]
            x0=int(max(0,np.floor(p[:,0].min()))); x1=int(min(Wd-1,np.ceil(p[:,0].max())))
            y0=int(max(0,np.floor(p[:,1].min()))); y1=int(min(H-1,np.ceil(p[:,1].max())))
            if x1<x0 or y1<y0: continue
            xs,ys=np.meshgrid(np.arange(x0,x1+1)+0.5,np.arange(y0,y1+1)+0.5)
            (ax,ay,az),(bx,by,bz),(cx,cy,cz)=p
            den=(by-cy)*(ax-cx)+(cx-bx)*(ay-cy)
            if abs(den)<1e-9: continue
            w1=((by-cy)*(xs-cx)+(cx-bx)*(ys-cy))/den
            w2=((cy-ay)*(xs-cx)+(ax-cx)*(ys-cy))/den
            w3=1-w1-w2
            m=(w1>=-1e-4)&(w2>=-1e-4)&(w3>=-1e-4)
            if not m.any(): continue
            z=w1*az+w2*bz+w3*cz
            sub=zb[y0:y1+1,x0:x1+1]
            m&=z>sub
            sub[m]=z[m]
            img[y0:y1+1,x0:x1+1][m]=np.array(col)*shade[t]
    for (pt,colm,r) in marks:
        q=proj(np.array([pt]))[0]
        x,y=int(q[0]),int(q[1])
        yy,xx=np.ogrid[-r:r+1,-r:r+1]; disk=xx*xx+yy*yy<=r*r
        for dy in range(-r,r+1):
            for dx in range(-r,r+1):
                if dx*dx+dy*dy<=r*r and 0<=y+dy<H and 0<=x+dx<Wd: img[y+dy,x+dx]=colm
    return np.clip(img,0,255).astype(np.uint8),(c,span)
