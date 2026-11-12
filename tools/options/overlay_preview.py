"""Preview of the OPTIONS screen's overlay (text, cursor, Yes / No) over the wall, without a Windows build.

python tools/options/overlay_preview.py out.png [--size 1920x1080] [--title] [--cursor N] [--confirm N]
    [--blur 0.5] [--shadow 0.18]

Builds tools/options/overlay_preview.cpp (g++, the repo's ImGui) with the DLL's own drawing code cut from HostStubs.cpp,
crops resources/options/wall.jpg to the size's shape and draws options_overlay.cpp's list over it. Needs Pillow.
"""
import argparse, os, re, subprocess, tempfile
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMGUI = os.path.join(ROOT, 'thirdparty/pcsx2/3rdparty/imgui')

def build(work):
    stubs = open(os.path.join(ROOT, 'thirdparty/pcsx2-gs/compat/HostStubs.cpp')).read()
    m = re.search(r"// --- the port's own screen \(pcsx2gs_set_screen\).*?(?=// The port's overlays \(the host loop)", stubs, re.S)
    open(os.path.join(work, 'screen_snippet.inc'), 'w').write(m.group(0))
    exe = os.path.join(work, 'overlay_preview')
    subprocess.check_call(['g++', '-std=c++20', '-O2', '-DIMGUI_USER_CONFIG="pcsx2gs_imconfig.h"', '-I' + work,
                           '-I' + os.path.join(ROOT, 'thirdparty/pcsx2-gs/compat'), '-I' + os.path.join(IMGUI, 'include'),
                           '-I' + os.path.join(ROOT, 'thirdparty/pcsx2-gs/include'), '-I' + os.path.join(ROOT, 'src'),
                           '-o', exe, os.path.join(ROOT, 'tools/options/overlay_preview.cpp'),
                           os.path.join(ROOT, 'src/kamite/options_overlay.cpp')] +
                          [os.path.join(IMGUI, 'src', f) for f in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_widgets.cpp', 'imgui_tables.cpp')])
    return exe

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--size', default='1920x1080')
    ap.add_argument('--title', action='store_true', help='the title screen\'s list (else the pause\'s)')
    ap.add_argument('--cursor', type=int, default=0)
    ap.add_argument('--confirm', type=int, default=-1, help='row asking Yes / No')
    ap.add_argument('--blur', type=float, default=0.5)
    ap.add_argument('--shadow', type=float, default=0.18)
    a = ap.parse_args()
    w, h = map(int, a.size.split('x'))
    work = tempfile.mkdtemp()
    exe = build(work)
    wall = Image.open(os.path.join(ROOT, 'resources/options/wall.jpg')).convert('RGB')
    cw = min(wall.width, int(wall.height * w / h))
    x = (wall.width - cw) // 2
    raw_in, raw_out = os.path.join(work, 'wall.rgb'), os.path.join(work, 'out.rgb')
    open(raw_in, 'wb').write(wall.crop((x, 0, x + cw, wall.height)).resize((w, h), Image.LANCZOS).tobytes())
    fonts = os.path.join(ROOT, 'resources/fonts')
    subprocess.check_call([exe, raw_in, str(w), str(h), raw_out, '0' if a.title else '1', str(a.cursor), str(a.confirm),
                           str(a.blur), str(a.shadow), os.path.join(fonts, 'ArchivoNarrow-SemiBold.ttf'),
                           os.path.join(fonts, 'DejaVuSans-Bold.ttf')])
    Image.frombytes('RGB', (w, h), open(raw_out, 'rb').read()).save(a.out)

if __name__ == '__main__':
    main()
