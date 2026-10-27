import os
WORK=os.environ.get('XAVIER_WORK','xavier_work')
# Blender: stage1 -> decimated, material-grouped meshes + textures, dumped as JSON for encoding.
import bpy, bmesh, sys, json, math, os, itertools
import numpy as np
from mathutils import Vector
OUT=WORK
TEX=WORK+'/textures'
bpy.ops.wm.open_mainfile(filepath=OUT+'/xavier_stage1.blend')

def obj(n): return bpy.data.objects['X_'+n]
def decimate(o, ratio):
    if ratio>=1: return
    bpy.context.view_layer.objects.active=o
    m=o.modifiers.new('dec','DECIMATE'); m.ratio=ratio; m.use_collapse_triangulate=True
    bpy.ops.object.modifier_apply(modifier='dec')
def tri(o):
    bm=bmesh.new(); bm.from_mesh(o.data); bmesh.ops.triangulate(bm,faces=bm.faces[:]); bm.to_mesh(o.data); bm.free()

budget={'XavierBody':0.5,'XavierHair':0.55,'XavierNecklace':0.6,'XavierShoulderPad':1.0,'Xaviersnakeeyes':0.5,'XavierEyeR':0.03,'XavierEyesL':0.03}
# The snake's eyes are painted into the body texture (K's XavierDiffuse.png: unpack_textures.py), so their spheres go (XAVIER_SNAKE_EYES=geometry
# keeps them). The snake head (stored four times: encode_xavier) keeps the body's ratio (SNAKE_KEEP 1); its teeth and
# tongue are taken out of the body before the decimation (snake_bits): each tooth becomes a three-sided pyramid
# (its root and tip kept), the tongue keeps TONGUE_KEEP of its faces.
if os.environ.get('XAVIER_SNAKE_EYES','paint')!='geometry':
    del budget['Xaviersnakeeyes']
SNAKE_KEEP=float(os.environ.get('XAVIER_SNAKE_KEEP','1'))
TONGUE_KEEP=float(os.environ.get('XAVIER_TONGUE_KEEP','0.5'))

def components(bm, faces):
    seen=set(); out=[]
    for f in faces:
        if f.index in seen: continue
        st=[f]; comp=[]
        while st:
            g=st.pop()
            if g.index in seen: continue
            seen.add(g.index); comp.append(g)
            for v in g.verts:
                for h in v.link_faces:
                    if h.index not in seen: st.append(h)
        out.append(comp)
    return out

def snake_bits():
    """Moves the snake's teeth and tongue (the small separate parts of the body mesh on the snake's bones) into their
    own object X_XavierSnakeBits: the teeth as pyramids, the tongue decimated to TONGUE_KEEP. Returns its name."""
    o=obj('XavierBody')
    ids=[g.index for g in o.vertex_groups if g.name in ('16','31')]
    bm=bmesh.new(); bm.from_mesh(o.data); bm.faces.ensure_lookup_table(); bm.verts.ensure_lookup_table()
    dl=bm.verts.layers.deform.active
    on=lambda v: sum(w for g,w in v[dl].items() if g in ids)>0.3
    parts=[c for c in components(bm, bm.faces) if len(c)<200 and all(on(v) for f in c for v in f.verts)]
    if not parts: bm.free(); return None
    keep=set(f.index for c in parts for f in c)
    bits=o.copy(); bits.data=o.data.copy(); bits.name='X_XavierSnakeBits'; bpy.context.scene.collection.objects.link(bits)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.index in keep], context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bm.to_mesh(o.data); bm.free()
    bm=bmesh.new(); bm.from_mesh(bits.data); bm.faces.ensure_lookup_table()
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.index not in keep], context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bm.faces.ensure_lookup_table(); bm.verts.ensure_lookup_table()
    uvl=bm.loops.layers.uv.active
    comps=sorted(components(bm, bm.faces), key=len)
    comps.pop()   # the largest part: the tongue
    teeth=0
    for c in comps:
        vs=list(set(v for f in c for v in f.verts))
        P=np.array([tuple(v.co) for v in vs]); m=P.mean(0)
        ax=np.linalg.svd(P-m)[2][0]
        t=(P-m)@ax
        # the tip: the end of the long axis with the fewest vertices near it (a cone's point)
        span=t.max()-t.min()
        lo=(t<t.min()+0.2*span).sum(); hi=(t>t.max()-0.2*span).sum()
        if lo<hi: ax=-ax; t=-t
        tip=vs[int(np.argmax(t))]
        # the root: the three vertices of the root half that span the widest triangle (the tooth's full base)
        # the root: the three vertices of the root half that span the widest triangle (the tooth's full base)
        root=[i for i in range(len(vs)) if t[i]<t.min()+0.5*span]
        area=lambda a,b,c: np.linalg.norm(np.cross(P[b]-P[a],P[c]-P[a]))
        ring=[vs[i] for i in max(itertools.combinations(root,3), key=lambda abc: area(*abc))]
        uv={v:v.link_loops[0][uvl].uv.copy() for v in ring+[tip]}
        mat=c[0].material_index
        bmesh.ops.delete(bm, geom=c, context='FACES_ONLY')
        for a,b in ((ring[0],ring[1]),(ring[1],ring[2]),(ring[2],ring[0])):
            n=np.cross(np.array(b.co)-np.array(a.co),np.array(tip.co)-np.array(a.co))
            out=(np.array(a.co)+np.array(b.co))/2-(m+ax*(((np.array(a.co)+np.array(b.co))/2-m)@ax))
            f=bm.faces.new((a,b,tip) if n@out>=0 else (b,a,tip)); f.material_index=mat
            for l in f.loops: l[uvl].uv=uv[l.vert]
        teeth+=1
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bm.to_mesh(bits.data); bm.free()
    tri(bits)
    total=len(bits.data.polygons)
    tg=bits.vertex_groups.new(name='tonguedec')
    # the tongue: the vertices of the parts that are not teeth (a tooth is 3 triangles now)
    tv=set()
    bm=bmesh.new(); bm.from_mesh(bits.data); bm.faces.ensure_lookup_table()
    for c in components(bm, bm.faces):
        if len(c)>3: tv|=set(v.index for f in c for v in f.verts)
    tongue_faces=sum(1 for c in components(bm,bm.faces) if len(c)>3 for f in c)
    bm.free()
    for i in tv: tg.add([i],1.0,'REPLACE')
    if TONGUE_KEEP<1 and tongue_faces:
        m=bits.modifiers.new('dectongue','DECIMATE'); m.ratio=(total-tongue_faces*(1-TONGUE_KEEP))/total; m.use_collapse_triangulate=True
        m.vertex_group='tonguedec'; m.vertex_group_factor=10.0
        bpy.context.view_layer.objects.active=bits; bpy.ops.object.modifier_apply(modifier='dectongue')
    print('SNAKE BITS: %d teeth as pyramids, tongue %d -> %d faces, %d faces in all' % (teeth, tongue_faces, len(bits.data.polygons)-3*teeth, len(bits.data.polygons)))
    return 'XavierSnakeBits'

def mouth_teeth():
    """Moves Xavier's own teeth (the white parts of the mouth interior: prep1's 'mouthdec' faces whose texture is the
    tooth white of XavierFace.png's bottom strip) into an object X_XavierTeeth, each tooth rebuilt as a band of three
    quads: the front, the biting edge and the back (6 triangles; the sides stay open, so the teeth read as separate).
    Each tooth keeps its place, size (TOOTH_WIDTH of its width) and bone; the gum and the rest of the interior keep
    MOUTH_KEEP of their faces. Returns the object's name."""
    o=obj('XavierBody')
    if 'mouthdec' not in o.vertex_groups: return None
    gi=o.vertex_groups['mouthdec'].index
    from PIL import Image
    T=np.asarray(Image.open(os.path.join(TEX,'XavierFace.png')).convert('RGB')).astype(int); TH,TW=T.shape[:2]
    bm=bmesh.new(); bm.from_mesh(o.data); bm.faces.ensure_lookup_table(); bm.verts.ensure_lookup_table()
    dl=bm.verts.layers.deform.active; uvl=bm.loops.layers.uv.active
    def white(f):
        u=np.mean([l[uvl].uv[0] for l in f.loops]); v=np.mean([l[uvl].uv[1] for l in f.loops])
        return T[int(np.clip((1-v)*TH,0,TH-1)),int(np.clip(u*TW,0,TW-1))].min()>200
    tooth=[f for f in bm.faces if all(v[dl].get(gi,0)>0.5 for v in f.verts) and white(f)]
    ts=set(f.index for f in tooth); seen=set(); comps=[]
    for f in tooth:
        if f.index in seen: continue
        st=[f]; c=[]
        while st:
            g=st.pop()
            if g.index in seen: continue
            seen.add(g.index); c.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h.index in ts and h.index not in seen: st.append(h)
        comps.append(c)
    comps=[c for c in comps if len(c)>=8]       # crumbs of white elsewhere in the interior stay where they are
    if not comps: bm.free(); return None
    allp=np.array([tuple(v.co) for c in comps for f in c for v in f.verts])
    centre=np.array((0.0, allp[:,1].max()+0.02))  # behind the arch (Blender -y is the front)
    me=bpy.data.meshes.new('XavierTeeth'); teeth=bpy.data.objects.new('X_XavierTeeth', me)
    bpy.context.scene.collection.objects.link(teeth)
    for g in o.vertex_groups: teeth.vertex_groups.new(name=g.name)
    for sl in o.material_slots: teeth.data.materials.append(sl.material)
    tb=bmesh.new(); tdl=tb.verts.layers.deform.verify(); tuv=tb.loops.layers.uv.new()
    built=0
    for c in comps:
        vs=list(set(v for f in c for v in f.verts))
        P=np.array([tuple(v.co) for v in vs]); m=P.mean(0)
        r=np.array((m[0]-centre[0], m[1]-centre[1], 0.0)); r/=np.linalg.norm(r)    # out of the arch (towards the lips)
        t=np.cross((0,0,1),r); z=np.array((0,0,1.0))
        lt,lr,lz=(P-m)@t,(P-m)@r,(P-m)@z
        w=(lt.max()-lt.min())*TOOTH_WIDTH/2
        upper=sum(v[dl].get(o.vertex_groups['4'].index,0) for v in vs)>sum(v[dl].get(o.vertex_groups['26'].index,0) for v in vs)
        root,edge=(lz.max(),lz.min()) if upper else (lz.min(),lz.max())
        front,back=lr.max()+TOOTH_FORWARD,lr.min()+TOOTH_FORWARD
        # towards the lips' opening: the upper row down, the lower row up
        root,edge=(root-TOOTH_SHIFT,edge-TOOTH_SHIFT) if upper else (root+TOOTH_SHIFT,edge+TOOTH_SHIFT)
        # corners: front-root, front-edge, back-edge, back-root, at both sides (-w, +w)
        prof=[(front,root),(front,edge),(back,edge),(back,root)]
        def near(p):
            return vs[int(np.argmin(np.linalg.norm(P-p,axis=1)))]
        ring=[]
        for k,(rr,zz) in enumerate(prof):
            row=[]
            for side in (-w,w):
                p=m+t*side+r*rr+z*zz
                src=near(p)
                nv=tb.verts.new(p)
                for g,wt in src[dl].items(): nv[tdl][g]=wt
                row.append((nv,src.link_loops[0][uvl].uv.copy()))
            ring.append(row)
        for k in range(3):
            (a,ua),(b,ub)=ring[k]; (c_,uc),(d,ud)=ring[k+1]
            f=tb.faces.new((a,c_,d,b))
            # wind outwards: front faces +r, the edge away from the root, back faces -r
            out=(r if k==0 else -r if k==2 else z*(-1 if upper else 1))
            n=np.cross(np.array(c_.co)-np.array(a.co),np.array(b.co)-np.array(a.co))
            if n@out<0: f.normal_flip()
            f.material_index=c[0].material_index
            uvs={a:ua,b:ub,c_:uc,d:ud}
            for l in f.loops: l[tuv].uv=uvs[l.vert]
        built+=1
    tb.to_mesh(me); tb.free()
    bmesh.ops.delete(bm, geom=[f for c in comps for f in c], context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bm.to_mesh(o.data); bm.free()
    print('TEETH: %d teeth as bands (%d triangles)' % (built, 6*built))
    return 'XavierTeeth'

TOOTH_WIDTH=float(os.environ.get('XAVIER_TOOTH_WIDTH','0.85'))
# The model's teeth sit well inside the lips (only ~4 mm of the upper row can show): TOOTH_SHIFT moves both rows
# towards the opening between the lips, TOOTH_FORWARD towards the lips (metres).
TOOTH_SHIFT=float(os.environ.get('XAVIER_TOOTH_SHIFT','0.005'))
TOOTH_FORWARD=float(os.environ.get('XAVIER_TOOTH_FORWARD','0.003'))
if os.environ.get('XAVIER_SNAKE_TEETH','pyramids')=='pyramids':
    tri(obj('XavierBody'))
    _bits=snake_bits()
    if _bits: budget[_bits]=1.0
if os.environ.get('XAVIER_TEETH','bands')=='bands':
    tri(obj('XavierBody'))
    _teeth=mouth_teeth()
    if _teeth: budget[_teeth]=1.0
MOUTH_KEEP=float(os.environ.get('XAVIER_MOUTH_KEEP','0.08'))   # the mouth interior (prep1's 'mouthdec' group) keeps this share of its faces (an interior, seen
                  # through a half-open mouth); the rest of the body keeps its own budget ratio as before
for n,r in budget.items():
    o=obj(n); tri(o)
    if 'mouthdec' in o.vertex_groups:
        gi=o.vertex_groups['mouthdec'].index
        inside=sum(1 for p in o.data.polygons if all(any(g.group==gi and g.weight>0.5 for g in o.data.vertices[v].groups) for v in p.vertices))
        total=len(o.data.polygons); body=total-inside; mouth=int(inside*MOUTH_KEEP)
        m=o.modifiers.new('decmouth','DECIMATE'); m.ratio=(body+mouth)/total; m.use_collapse_triangulate=True
        m.vertex_group='mouthdec'; m.vertex_group_factor=10.0
        bpy.context.view_layer.objects.active=o; bpy.ops.object.modifier_apply(modifier='decmouth')
        print('DEC mouth', n, inside, 'of', total, 'faces ->', len(o.data.polygons))
        r=(body*r+mouth)/len(o.data.polygons)   # the body's share decimated as without the mouth
    decimate(o,r); print('DEC',n,len(o.data.polygons))
    if n=='XavierBody' and SNAKE_KEEP<1:
        # The snake's head is stored four times (Gene's four left-hand meshes, encode_xavier): it keeps SNAKE_KEEP of
        # its faces after the body's decimation, so the four copies cost about what the hand meshes they replace did.
        g16=o.vertex_groups.get('16'); g31=o.vertex_groups.get('31')
        ids=[g.index for g in (g16,g31) if g]
        sg=o.vertex_groups.new(name='snakedec')
        for v in o.data.vertices:
            w=sum(g.weight for g in v.groups if g.group in ids)
            if w>0.3: sg.add([v.index],1.0,'REPLACE')
        inside=sum(1 for p in o.data.polygons if all(any(g.group==sg.index for g in o.data.vertices[v].groups) for v in p.vertices))
        total=len(o.data.polygons)
        m=o.modifiers.new('decsnake','DECIMATE'); m.ratio=(total-inside*(1-SNAKE_KEEP))/total; m.use_collapse_triangulate=True
        m.vertex_group='snakedec'; m.vertex_group_factor=10.0
        bpy.context.view_layer.objects.active=o; bpy.ops.object.modifier_apply(modifier='decsnake')
        print('DEC snake head', inside, 'faces of', total, '->', len(o.data.polygons))

images={}
def img_array(name):
    if name not in images:
        from PIL import Image
        images[name]=np.asarray(Image.open(os.path.join(TEX,name)).convert('RGBA'))
    return images[name]

matimg={'body':'XavierDiffuse.png','head':'XavierFace.png','hair':'Xavier_HAIR.png','eyeR':'Eye_BLUE.png','eyeL':'Eye_BROWN.png','snakeeye':'Xavier_SNAKEEYE.png'}

# collect triangles: (obj, poly) with material name and loop data
faces=[]  # dict(mat, verts=[(pos, uv, weights)], normals per corner)
for n in budget:
    o=obj(n); me=o.data
    me.calc_loop_triangles() if hasattr(me,'calc_loop_triangles') else None
    uvl=me.uv_layers.active.data
    vg={g.index:int(g.name) for g in o.vertex_groups if g.name.isdigit()}
    cl=me.attributes.get('cloth')
    for p in me.polygons:
        mat=o.material_slots[p.material_index].material.name
        corners=[]
        for li in p.loop_indices:
            l=me.loops[li]; v=me.vertices[l.vertex_index]
            w=sorted([(vg[g.group],g.weight) for g in v.groups if g.weight>0.0005 and g.group in vg], key=lambda x:-x[1])[:3]
            corners.append(dict(key=(n,l.vertex_index), pos=tuple(v.co), nrm=tuple(v.normal), uv=tuple(uvl[li].uv), w=w, cloth=(cl.data[l.vertex_index].value if cl else 0.0)))
        faces.append(dict(obj=n,mat=mat,c=corners))
print('faces',len(faces))

# ---- split the body atlas into colour groups by UV island
from collections import defaultdict
body=[f for f in faces if f['mat']=='body']
# islands: union faces sharing a corner with identical (vertex, uv)
parent=list(range(len(body)))
def find(a):
    while parent[a]!=a: parent[a]=parent[parent[a]]; a=parent[a]
    return a
owner={}
for i,f in enumerate(body):
    for c in f['c']:
        k=(c['key'],round(c['uv'][0],4),round(c['uv'][1],4))
        if k in owner: parent[find(i)]=find(owner[k])
        else: owner[k]=i
isl=defaultdict(list)
for i in range(len(body)): isl[find(i)].append(i)
A=img_array(matimg['body']).astype(np.float32); H,W=A.shape[:2]
def sample(uv):
    x=min(W-1,max(0,int(uv[0]%1*W))); y=min(H-1,max(0,int((1-uv[1]%1)*H)))
    return A[y,x,:3]
feat=[]; keys=list(isl)
for k in keys:
    cols=[sample(c['uv']) for i in isl[k] for c in body[i]['c']]
    # also sample face centroids
    for i in isl[k]:
        u=sum(c['uv'][0] for c in body[i]['c'])/3; v=sum(c['uv'][1] for c in body[i]['c'])/3
        cols.append(sample((u,v)))
    cols=np.array(cols); feat.append((np.median(cols,0), len(isl[k])))
loin_isl=set()
for k in keys:
    cs=[np.mean([c['pos'] for c in body[i]['c']],0) for i in isl[k]]
    inz=[ (0.84<=c[2]<=1.2 and abs(c[0])<0.17) for c in cs]
    frac=sum(inz)/len(inz)
    if frac>0.3: print('cand',round(frac,2),len(isl[k]),np.min(cs,0).round(2), np.max(cs,0).round(2), feat[keys.index(k)][0].round())
    if frac>0.9 and np.ptp(np.array(cs)[:,1])<0.15:
        loin_isl.add(k); print('LOIN island',len(isl[k]),'faces', np.min(cs,0).round(2), np.max(cs,0).round(2), feat[keys.index(k)][0].round())
for k in loin_isl:
    for i in isl[k]: body[i]['mat']='loin'
keys=[k for k in keys if k not in loin_isl]
feat=[f for k,f in zip(list(isl),feat) if k not in loin_isl]
# simple k-means in RGB on island medians, weighted
K=5
X=np.array([f[0] for f in feat]); Wt=np.array([f[1] for f in feat],float)
rng=np.random.default_rng(1)
cent=X[np.argsort(-Wt)[:1]]
while len(cent)<K:
    d=np.min(((X[:,None,:]-cent[None])**2).sum(-1),1)*Wt
    cent=np.vstack([cent,X[np.argmax(d)]])
for it in range(30):
    lab=np.argmin(((X[:,None,:]-cent[None])**2).sum(-1),1)
    for j in range(K):
        if (lab==j).any(): cent[j]=np.average(X[lab==j],0,Wt[lab==j])
for j in range(K): print('GROUP',j,cent[j].round(),int(Wt[lab==j].sum()),'faces')
fur=int(np.argmax([Wt[lab==j].sum() for j in range(K)]))
rest=[i for i in range(len(keys)) if lab[i]!=fur]
cen=[]
for i in rest:
    uv=np.array([c['uv'] for f in isl[keys[i]] for c in body[f]['c']]); cen.append(uv.mean(0))
cen=np.array(cen); K2=2
c2=cen[np.linspace(0,len(cen)-1,K2).astype(int)]
for it in range(50):
    l2=np.argmin(((cen[:,None,:]-c2[None])**2).sum(-1),1)
    for j in range(K2):
        if (l2==j).any(): c2[j]=cen[l2==j].mean(0)
for i in range(len(keys)):
    for f in isl[keys[i]]: body[f]['mat']='body0'
for i,l in zip(rest,l2):
    for f in isl[keys[i]]: body[f]['mat']='body%d'%(l+1)
json.dump(dict(faces=faces,groups=[c.tolist() for c in cent]),open(OUT+'/xavier_faces.json','w'))
