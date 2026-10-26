"""A game model as a rigged glTF binary (.glb): mesh, skeleton, skin weights and textures, for animation tools
(Cinevva, Mixamo, Blender) to animate. What they give back goes into the game through npc_rail.py's retargeting.

    python -I tools/models/export_glb.py <file.dat> <MD entry> <TM3 entry> <out.glb> [--nodes a,b,c] [--set suffix]

    e.g. the casino's bunny girl: em/ema5.dat 15 14 bunny.glb --nodes body,HD_00,RH_00,LH_00,z_00,z_02,z_04

The model's bones are the glTF joints in the game's order (bone i = joint i, named bone_NN; the woman's skeleton:
0 hips, 1 waist, 2 chest, 3 neck, 4 head, 5/11 shoulders, 7/13 upper arms, 8/14 forearms, 9/15 hands, 10/16 fingers,
17 pelvis, 18-21 / 22-25 legs to toes, 26 head attachment, 29 the bust). The rest pose is the game's: every bone's
rotation zero, a T-pose, Y up, facing +Z, in metres. Each material's texture is decoded from the TM3 (material n =
the TM3's n-th texture, or the one named <name><suffix> with --set, e.g. --set a for the alternate colours).
Needs numpy and Pillow. The file holds the game's art: it stays private, like the rest of what is built from the disc.
"""
import io, json, os, struct, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'xavier'))
from gamemodel import dat_read, scr_read, mdb_read, tm3_read, strip_tris  # noqa: E402
from gametex import tim3_decode  # noqa: E402


def export(dat_path, md_index, tm3_index, out, only=None, suffix=''):
    ents = dat_read(open(dat_path, 'rb').read())
    md = ents[md_index][1]
    kind, nodes = scr_read(md)
    bones = mdb_read(md, nodes[0]['mdb_off'])['bones']
    rest = []
    for i, b in enumerate(bones):
        p = b['parent']
        rest.append(np.array(b['t']) + (rest[p] if 0 <= p < i else 0))
    texs = tm3_read(ents[tm3_index][1])
    by_name = {n: d for n, d, o in texs}

    prims = {}  # material -> lists
    for nd in nodes:
        if only and nd['name'] not in only:
            continue
        m = mdb_read(md, nd['mdb_off'])
        for vb in m['vbs']:
            if not vb['wt']:
                continue
            p = prims.setdefault(vb['mat'], dict(pos=[], nrm=[], uv=[], j=[], w=[], idx=[]))
            base = len(p['pos'])
            for k in range(vb['count']):
                p['pos'].append(vb['pos'][k])
                n = np.array(vb['nrm'][k], float) / 127.0 if vb['nrm'] else np.array([0, 1.0, 0])
                p['nrm'].append(n / (np.linalg.norm(n) or 1.0))
                u, v = vb['uv'][k]
                p['uv'].append((u / 4096.0, 1.0 + v / 4096.0))  # the game's v counts down from the image's bottom
                jb, wb = vb['wt'][k]
                joints = [jb[1] // 4, jb[2] // 4, jb[3] // 4, 0]
                weights = [wb[0] / 100.0, wb[1] / 100.0, wb[2] / 100.0, 0.0]
                s = sum(weights) or 1.0
                p['j'].append(joints)
                p['w'].append([x / s for x in weights])
            for a, b2, c in strip_tris(vb['flags']):
                p['idx'] += [base + a, base + b2, base + c]

    blob = bytearray()
    views, accessors = [], []

    def add(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(data), **({'target': target} if target else {})))
        blob.extend(data)
        return len(views) - 1

    def accessor(arr, ctype, typ, target=None, minmax=False):
        a = dict(bufferView=add(arr.tobytes(), target), componentType=ctype, count=len(arr), type=typ)
        if minmax:
            a['min'] = arr.min(0).tolist()
            a['max'] = arr.max(0).tolist()
        accessors.append(a)
        return len(accessors) - 1

    images, textures, materials, primitives = [], [], [], []
    for mat in sorted(prims):
        p = prims[mat]
        name = texs[mat][0] if mat < len(texs) else None
        if name and suffix and (name + suffix) in by_name:
            name = name + suffix
        mi = None
        if name:
            rgba, _ = tim3_decode(by_name[name])
            buf = io.BytesIO()
            Image.fromarray(rgba, 'RGBA').save(buf, 'PNG')
            images.append(dict(bufferView=add(buf.getvalue()), mimeType='image/png', name=name))
            textures.append(dict(source=len(images) - 1, sampler=0))
            cut = bool((rgba[..., 3] < 250).any())
            materials.append(dict(name=name, doubleSided=True, alphaMode='MASK' if cut else 'OPAQUE',
                                  **({'alphaCutoff': 0.5} if cut else {}),
                                  pbrMetallicRoughness=dict(baseColorTexture=dict(index=len(textures) - 1),
                                                            metallicFactor=0.0, roughnessFactor=1.0)))
            mi = len(materials) - 1
        attrs = dict(POSITION=accessor(np.array(p['pos'], np.float32), 5126, 'VEC3', 34962, True),
                     NORMAL=accessor(np.array(p['nrm'], np.float32), 5126, 'VEC3', 34962),
                     TEXCOORD_0=accessor(np.array(p['uv'], np.float32), 5126, 'VEC2', 34962),
                     JOINTS_0=accessor(np.array(p['j'], np.uint8), 5121, 'VEC4', 34962),
                     WEIGHTS_0=accessor(np.array(p['w'], np.float32), 5126, 'VEC4', 34962))
        prim = dict(attributes=attrs, indices=accessor(np.array(p['idx'], np.uint32), 5125, 'SCALAR', 34963))
        if mi is not None:
            prim['material'] = mi
        primitives.append(prim)

    gnodes = []
    for i, b in enumerate(bones):
        gnodes.append(dict(name='bone_%02d' % i, translation=[float(x) for x in b['t']]))
    for i, b in enumerate(bones):
        p = b['parent']
        if 0 <= p < len(bones) and p != i:
            gnodes[p].setdefault('children', []).append(i)
    roots = [i for i, b in enumerate(bones) if not (0 <= b['parent'] < len(bones)) or b['parent'] == i]
    ibm = np.zeros((len(bones), 16), np.float32)
    for i in range(len(bones)):
        m = np.eye(4, dtype=np.float32)
        m[:3, 3] = -rest[i]
        ibm[i] = m.T.reshape(-1)  # column-major
    mesh_node = len(gnodes)
    gnodes.append(dict(name='model', mesh=0, skin=0))
    root = len(gnodes)
    gnodes.append(dict(name='armature', children=roots))
    skin = dict(joints=list(range(len(bones))), inverseBindMatrices=accessor(ibm, 5126, 'MAT4'), skeleton=root)
    gltf = dict(asset=dict(version='2.0', generator='Kamite tools/models/export_glb.py'), scene=0,
                scenes=[dict(nodes=[root, mesh_node])], nodes=gnodes, meshes=[dict(name='model', primitives=primitives)],
                skins=[skin], materials=materials, textures=textures, images=images,
                samplers=[dict(magFilter=9729, minFilter=9987, wrapS=10497, wrapT=10497)],
                accessors=accessors, bufferViews=views, buffers=[dict(byteLength=0)])
    while len(blob) % 4:
        blob.append(0)
    gltf['buffers'][0]['byteLength'] = len(blob)
    js = json.dumps(gltf, separators=(',', ':')).encode()
    js += b' ' * ((4 - len(js) % 4) % 4)
    with open(out, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
        f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
        f.write(struct.pack('<II', len(blob), 0x004E4942) + bytes(blob))
    return len(primitives), len(bones)


if __name__ == '__main__':
    args = sys.argv[1:]
    only, suffix = None, ''
    if '--nodes' in args:
        i = args.index('--nodes'); only = set(args[i + 1].split(',')); del args[i:i + 2]
    if '--set' in args:
        i = args.index('--set'); suffix = args[i + 1]; del args[i:i + 2]
    print(export(args[0], int(args[1]), int(args[2]), args[3], only, suffix))
