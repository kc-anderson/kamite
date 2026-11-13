"""Preview of the OPTIONS screen's overlay (text, cursor, Yes / No) over the wall, without a Windows build.

python tools/options/overlay_preview.py out.png [--size 1920x1080] [--title] [--cursor N] [--confirm N]
    [--page gameplay|interface|sound] [--open] [--row N] [--scroll N]

Builds tools/options/overlay_preview.cpp (g++, the repo's ImGui) with the DLL's own drawing code cut from HostStubs.cpp,
crops resources/options/wall.jpg to the size's shape and draws options_overlay.cpp's list over it. Needs Pillow.
"""
import argparse, os, re, struct, subprocess, tempfile
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
    ap.add_argument('--page', choices=['gameplay', 'interface', 'sound'], help='the mockup page beside the list (or open)')
    ap.add_argument('--open', action='store_true', help='the cursor in the page')
    ap.add_argument('--row', type=int, default=1, help="the page's cursor row")
    ap.add_argument('--scroll', type=int, default=0)
    a = ap.parse_args()
    w, h = map(int, a.size.split('x'))
    work = tempfile.mkdtemp()
    exe = build(work)
    wall = Image.open(os.path.join(ROOT, 'resources/options/wall.jpg')).convert('RGB')
    cw = min(wall.width, int(wall.height * w / h))
    x = (wall.width - cw) // 2
    raw_in, raw_out = os.path.join(work, 'wall.rgb'), os.path.join(work, 'out.rgb')
    open(raw_in, 'wb').write(wall.crop((x, 0, x + cw, wall.height)).resize((w, h), Image.LANCZOS).tobytes())
    title = work  # the titles' folder: each title_<name>.png handed over as raw RGBA
    for name in os.listdir(os.path.join(ROOT, 'resources/options')):
        if name.startswith('title_') and name.endswith('.png'):
            im = Image.open(os.path.join(ROOT, 'resources/options', name)).convert('RGBA')
            open(os.path.join(work, name) + '.rgba', 'wb').write(struct.pack('<II', *im.size) + im.tobytes())
    bars = []
    for name in ('scroll_track', 'scroll_thumb', 'slider_fill'):
        path = os.path.join(work, name + '.png')
        im = Image.open(os.path.join(ROOT, 'resources/options', name + '.png')).convert('RGBA')
        open(path + '.rgba', 'wb').write(struct.pack('<II', *im.size) + im.tobytes())
        bars.append(path)
    fonts = os.path.join(ROOT, 'resources/fonts')
    page = {'gameplay': 0, 'interface': 1, 'sound': 2}.get(a.page, -1)
    subprocess.check_call([exe, raw_in, str(w), str(h), raw_out, '0' if a.title else '1', str(a.cursor), str(a.confirm),
                           os.path.join(fonts, 'ArchivoNarrow-SemiBold.ttf'),
                           os.path.join(fonts, 'DejaVuSans-Bold.ttf'), title] + bars[:2] +
                          [str(page), '1' if a.open else '0', str(a.row), str(a.scroll), bars[2]])
    Image.frombytes('RGB', (w, h), open(raw_out, 'rb').read()).save(a.out)

if __name__ == '__main__':
    main()
