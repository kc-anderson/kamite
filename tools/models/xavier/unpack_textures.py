"""Xavier's textures out of the blend, as the PNGs prep1/prep2/encode_xavier read from <work>/textures:

    python tools/models/xavier/unpack_textures.py      (Blender as a Python module; XAVIER_BLEND, XAVIER_WORK)

Every packed image is saved as <name>.png. XAVIER_PAINTED (optional) is a folder of hand-painted versions under the
same names (e.g. XavierDiffuse.png with the snake's eyes painted into their sockets, Oct 27): those replace the
blend's. Run face_mouth.py after it (the mouth texture into the face's bottom strip). Needs Pillow."""
import io
import os
import shutil
import bpy
from PIL import Image

blend = os.environ['XAVIER_BLEND']
out = os.path.join(os.environ.get('XAVIER_WORK', 'xavier_work'), 'textures')
painted = os.environ.get('XAVIER_PAINTED')
os.makedirs(out, exist_ok=True)
bpy.ops.wm.open_mainfile(filepath=blend)
for im in bpy.data.images:
    if not im.packed_file:
        continue
    name = os.path.splitext(os.path.basename(im.filepath) or im.name)[0] + '.png'
    Image.open(io.BytesIO(im.packed_file.data)).save(os.path.join(out, name))
    print('unpacked', name)
if painted:
    for f in sorted(os.listdir(painted)):
        if f.lower().endswith('.png') and os.path.exists(os.path.join(out, f)):
            shutil.copyfile(os.path.join(painted, f), os.path.join(out, f))
            print('hand-painted', f)
