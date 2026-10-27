import os
WORK=os.environ.get('XAVIER_WORK','xavier_work')
# Blender: stage1 -> decimated, material-grouped meshes + textures, dumped as JSON for encoding.
import bpy, bmesh, sys, json, math, os
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
# tongue are taken out of the body before the decimation (snake_bits): each tooth becomes one flat two-sided
# triangle, the tongue is left out.
if os.environ.get('XAVIER_SNAKE_EYES','paint')!='geometry':
    del budget['Xaviersnakeeyes']
SNAKE_KEEP=float(os.environ.get('XAVIER_SNAKE_KEEP','1'))
SNAKE_TOOTH_WIDTH=float(os.environ.get('XAVIER_SNAKE_TOOTH_WIDTH','1.6'))    # the flat tooth's half-width / the cone's radius
SNAKE_TOOTH_LENGTH=float(os.environ.get('XAVIER_SNAKE_TOOTH_LENGTH','1.3'))  # the flat tooth's length / the cone's

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
    """Moves the snake's teeth (the small separate parts of the body mesh on the snake's bones) into their own object
    X_XavierSnakeBits as flat two-sided triangles; the tongue (the largest such part) is dropped. Returns its name."""
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
    two=bm.faces.layers.int.new('twosided')   # before any face is held: a new layer invalidates them
    dl=bm.verts.layers.deform.active
    bm.faces.ensure_lookup_table()
    comps=sorted(components(bm, bm.faces), key=len)
    tongue=comps.pop()   # the largest part: the tongue, left out (it showed through the shut jaw, K Oct 27)
    bmesh.ops.delete(bm, geom=tongue, context='FACES')
    # each tooth: its cone's axis (root to tip), root centre and radius
    info=[]
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
        root=[i for i in range(len(vs)) if t[i]<t.min()+0.35*span]
        rc=P[root].mean(0)
        rad=float(np.mean([np.linalg.norm((P[i]-rc)-ax*((P[i]-rc)@ax)) for i in root]))
        info.append((c,vs,P,ax,tip,root,rc,rad))
    centres=np.array([i[6] for i in info])
    lengths=np.array([float((np.array(i[4].co)-i[6])@i[3]) for i in info]); fang=np.median(lengths)*1.5
    teeth=0
    for k,(c,vs,P,ax,tip,root,rc,rad) in enumerate(info):
        L=float((np.array(tip.co)-rc)@ax)
        # one flat triangle along its row (its plane holds the tooth's axis and the line through its two nearest
        # neighbours; the fangs facing the sides), wider and longer than the cone, its base sunk into the gum; drawn from both sides
        # (encode_xavier: 'twosided')
        d=np.linalg.norm(centres-rc,axis=1); d[k]=np.inf
        n1,n2=np.argsort(d)[:2]
        row=centres[n1]-centres[n2] if np.linalg.norm(centres[n1]-centres[n2])>d[n1] else centres[n1]-rc
        tn=row-ax*(row@ax)
        if L>fang or np.linalg.norm(tn)<1e-9:
            tn=np.cross(ax,(0.0,1.0,0.0))   # the fangs stand apart from the rows: facing the head's sides (+-y)
        tn/=np.linalg.norm(tn)
        base=rc-ax*L*0.15
        pts=[base-tn*rad*SNAKE_TOOTH_WIDTH, base+tn*rad*SNAKE_TOOTH_WIDTH, rc+ax*L*SNAKE_TOOTH_LENGTH]
        src=[vs[min(root,key=lambda i: np.linalg.norm(P[i]-pts[0]))], vs[min(root,key=lambda i: np.linalg.norm(P[i]-pts[1]))], tip]
        new_v=[]
        for p_,sv in zip(pts,src):
            nv=bm.verts.new(p_)
            for g,w in sv[dl].items(): nv[dl][g]=w
            new_v.append((nv,sv.link_loops[0][uvl].uv.copy()))
        mat=c[0].material_index
        bmesh.ops.delete(bm, geom=c, context='FACES')
        f=bm.faces.new([v for v,_ in new_v]); f.material_index=mat; f[two]=1
        for l,(v,u) in zip(f.loops,new_v): l[uvl].uv=u
        teeth+=1
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bm.to_mesh(bits.data); bm.free()
    print('SNAKE BITS: %d teeth as two-sided triangles, the tongue (%d faces) left out' % (teeth, len(tongue)))
    return 'XavierSnakeBits'

def mouth_teeth():
    """Xavier's own teeth (the white parts of the mouth interior: prep1's 'mouthdec' faces whose texture is the tooth
    white of XavierFace.png's bottom strip; 30 separate teeth, ~2,000 triangles) replaced by two curved bands, K's
    call (Oct 27): the upper row a band behind the upper lip, the lower row one below the lower lip, each across the
    front of its row's arch (TOOTH_SPAN), following the arch's curve, front faces only, textured with the tooth strip
    face_mouth.py paints (gum at the root, teeth with gaps). Upper band on the head (bone 4), lower on the jaw (26).
    Moved TOOTH_SHIFT towards the opening between the lips and TOOTH_FORWARD forward, since the model keeps its teeth
    well inside the lips. Returns the new object's name."""
    o=obj('XavierBody')
    if 'mouthdec' not in o.vertex_groups: return None
    gi=o.vertex_groups['mouthdec'].index
    from PIL import Image
    # the tooth white in the unmodified mouth texture (face_mouth.py paints its tooth strip over the old teeth's art)
    T=np.asarray(Image.open(os.path.join(TEX,'MouthTexture1.png')).convert('RGB')).astype(int); TH,TW=T.shape[:2]
    bm=bmesh.new(); bm.from_mesh(o.data); bm.faces.ensure_lookup_table(); bm.verts.ensure_lookup_table()
    dl=bm.verts.layers.deform.active; uvl=bm.loops.layers.uv.active
    def white(f):
        u=np.mean([l[uvl].uv[0] for l in f.loops]); v=np.mean([l[uvl].uv[1] for l in f.loops])/MOUTH_V
        return T[int(np.clip((1-v)*TH,0,TH-1)),int(np.clip(u*TW,0,TW-1))].min()>200
    tooth=[f for f in bm.faces if all(v[dl].get(gi,0)>0.5 for v in f.verts) and white(f)]
    if not tooth: bm.free(); return None
    g4,g26=o.vertex_groups['4'].index,o.vertex_groups['26'].index
    rows={True:[],False:[]}   # upper (head), lower (jaw): the teeth's vertices
    for f in tooth:
        for v in f.verts:
            rows[v[dl].get(g4,0)>=v[dl].get(g26,0)].append(np.array(tuple(v.co)))
    mat=tooth[0].material_index
    bmesh.ops.delete(bm, geom=tooth, context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    bm.to_mesh(o.data); bm.free()
    me=bpy.data.meshes.new('XavierTeeth'); teeth=bpy.data.objects.new('X_XavierTeeth', me)
    bpy.context.scene.collection.objects.link(teeth)
    for g in o.vertex_groups: teeth.vertex_groups.new(name=g.name)
    for sl in o.material_slots: teeth.data.materials.append(sl.material)
    tb=bmesh.new(); tdl=tb.verts.layers.deform.verify(); tuv=tb.loops.layers.uv.new()
    for upper,P in rows.items():
        if len(P)<8: continue
        P=np.array(P)
        # the arch's front edge: per slice of x the frontmost point (Blender -y is the front), fitted y = a x^2 + c
        xs=np.linspace(P[:,0].min(),P[:,0].max(),12); fx=[]; fy=[]
        for x0,x1 in zip(xs[:-1],xs[1:]):
            m=(P[:,0]>=x0)&(P[:,0]<=x1)
            if m.any(): fx.append((x0+x1)/2); fy.append(P[m,1].min())
        A=np.vstack([np.array(fx)**2,np.ones(len(fx))]).T; a,c=np.linalg.lstsq(A,np.array(fy),rcond=None)[0]
        half=TOOTH_SPAN/2
        root,edge=(P[:,2].max(),P[:,2].min()) if upper else (P[:,2].min(),P[:,2].max())
        d=-TOOTH_SHIFT if upper else TOOTH_SHIFT
        root+=d; edge+=d
        bone=g4 if upper else g26
        N=8; cols=[]
        for k in range(N+1):
            x=-half+2*half*k/N; y=a*x*x+c-TOOTH_FORWARD*(1-(x/half)**2); col=[]   # forward in the middle, the ends on the arch (they showed at the corners of the shut mouth)
            for z,vv in ((root,0.0),(edge,1.0)):
                nv=tb.verts.new((x,y,z)); nv[tdl][bone]=1.0
                col.append((nv,(k/N, MOUTH_V*(1-TOOTH_STRIP*vv))))
            cols.append(col)
        for k in range(N):
            (r0,u0),(e0,w0)=cols[k]; (r1,u1),(e1,w1)=cols[k+1]
            f=tb.faces.new((r0,e0,e1,r1))
            f.normal_update()
            if f.normal.y>0: f.normal_flip()   # facing out of the mouth (-y)
            f.material_index=mat
            uvs={r0:u0,e0:w0,r1:u1,e1:w1}
            for l in f.loops: l[tuv].uv=uvs[l.vert]
    tb.to_mesh(me); tb.free()
    print('TEETH: %d tooth triangles replaced by two bands (%d triangles)' % (len(tooth), len(me.polygons)*2))
    return 'XavierTeeth'

TOOTH_SPAN=float(os.environ.get('XAVIER_TOOTH_SPAN','0.04'))    # metres across the front of each row
TOOTH_STRIP=52/512   # the tooth strip's height in the mouth texture (face_mouth.py)
MOUTH_V=0.38         # the mouth texture's share of XavierFace.png's height (prep1, face_mouth.py)
# The model's teeth sit well inside the lips (only ~4 mm of the upper row can show): TOOTH_SHIFT moves both rows
# towards the opening between the lips, TOOTH_FORWARD towards the lips (metres).
TOOTH_SHIFT=float(os.environ.get('XAVIER_TOOTH_SHIFT','0.005'))
TOOTH_FORWARD=float(os.environ.get('XAVIER_TOOTH_FORWARD','0.001'))
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
    cl=me.attributes.get('cloth'); ts=me.attributes.get('twosided')
    for p in me.polygons:
        mat=o.material_slots[p.material_index].material.name
        corners=[]
        for li in p.loop_indices:
            l=me.loops[li]; v=me.vertices[l.vertex_index]
            w=sorted([(vg[g.group],g.weight) for g in v.groups if g.weight>0.0005 and g.group in vg], key=lambda x:-x[1])[:3]
            corners.append(dict(key=(n,l.vertex_index), pos=tuple(v.co), nrm=tuple(v.normal), uv=tuple(uvl[li].uv), w=w, cloth=(cl.data[l.vertex_index].value if cl else 0.0)))
        faces.append(dict(obj=n,mat=mat,c=corners,ts=bool(ts and ts.data[p.index].value)))
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
