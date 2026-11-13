"""Preview of the OPTIONS screen's overlay (text, cursor, Yes / No) over the wall, without a Windows build.

python tools/options/overlay_preview.py out.png [--size 1920x1080] [--title] [--cursor N] [--confirm N]
    [--page controls|display|interface|gameplay|sound] [--open] [--row N] [--scroll N] [--set Section.Key=Value ...]
    [--menu western|japanese|classic] [--prompts xbox|playstation|nintendo|steam|keys]
    [--page mapping [--line PLACE] [--swap A:B ...] [--hot PLACE=NAME ...] [--readonly]]

Builds tools/options/overlay_preview.cpp (g++, the repo's ImGui) with the DLL's own drawing code cut from HostStubs.cpp,
crops resources/options/wall.jpg to the size's shape and draws options_overlay.cpp's list over it. The pages are the
game's own (options_pages.cpp from the shared settings list over the repo's GHR.ini; --set changes a value, e.g.
--set Game.ControlStyle=modern). --row is the page row (headers count). --menu / --prompts pick the confirm / cancel
hints' buttons (the default bindings: cross / circle / triangle on the bottom / right / top buttons, keys K / L / I).
--page mapping: CONTROLS > BUTTON MAPPING (mapping_page.cpp) for the --prompts set's pad over the repo's GHR.ini;
--line the selected button (south, east, l1 ...), --swap exchanges two buttons' actions, --hot puts a hotkey's name on a
button, --readonly shows a launcher layout. Needs Pillow.
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
                           '-o', exe, os.path.join(ROOT, 'tools/options/overlay_preview.cpp')] +
                          [os.path.join(ROOT, 'src/kamite', f) for f in ('options_overlay.cpp', 'options_pages.cpp',
                                                                         'settings_table.cpp', 'ini_file.cpp',
                                                                         'mapping_page.cpp')] +
                          [os.path.join(IMGUI, 'src', f) for f in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_widgets.cpp', 'imgui_tables.cpp')])
    return exe

# The default bindings' cross, circle and triangle in each set (prompt_hd.cpp's tables: Nintendo by place).
HINT_ART = {'xbox': ('xbox360', '360_A', '360_B', '360_Y', '360_LB', '360_RB'),
            'playstation': ('ps3', 'PS3_Cross', 'PS3_Circle', 'PS3_Triangle', 'PS3_L1', 'PS3_R1'),
            'nintendo': ('switch', 'Switch_B', 'Switch_A', 'Switch_X', 'Switch_LB', 'Switch_RB'),
            'steam': ('steamdeck', 'SteamDeck_A', 'SteamDeck_B', 'SteamDeck_Y', 'SteamDeck_L1', 'SteamDeck_R1'),
            'keys': ('keyboard', 'K_Key_Light', 'L_Key_Light', 'I_Key_Light', 'Q_Key_Light', 'E_Key_Light')}

def title_letters():
    """Each title's letters (alpha over half, as options_screen measures them): name:left:right;..."""
    out = []
    for name in ('controls', 'display', 'interface', 'gameplay', 'sound'):
        im = Image.open(os.path.join(ROOT, 'resources/options', 'title_%s.png' % name)).convert('RGBA')
        box = im.getchannel('A').point(lambda a: 255 if a > 127 else 0).getbbox()
        if box:
            out.append('%s:%d:%d' % (name, box[0], box[2]))
    return ';'.join(out)

def hint_paths(work, menu, prompts):
    folder, cross, circle, triangle, l1, r1 = HINT_ART[prompts]
    def path(name):
        out = os.path.join(work, name + '.png')
        im = Image.open(os.path.join(ROOT, 'resources/prompts/xelu', folder, name + '.png')).convert('RGBA')
        open(out + '.rgba', 'wb').write(struct.pack('<II', *im.size) + im.tobytes())
        return out
    shoulders = [path(l1), path(r1)]
    if menu == 'japanese':
        return [path(circle), '', path(cross)] + shoulders
    if menu == 'classic':
        return [path(cross), path(circle), path(triangle)] + shoulders
    return [path(cross), '', path(circle)] + shoulders

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--size', default='1920x1080')
    ap.add_argument('--title', action='store_true', help='the title screen\'s list (else the pause\'s)')
    ap.add_argument('--cursor', type=int, default=0)
    ap.add_argument('--confirm', type=int, default=-1, help='row asking Yes / No')
    ap.add_argument('--page', choices=['controls', 'display', 'interface', 'gameplay', 'sound', 'mapping'], help='the mockup page beside the list (or open)')
    ap.add_argument('--line', default='', help='mapping: the selected button')
    ap.add_argument('--swap', action='append', default=[], help='mapping: A:B exchanges two buttons\' actions')
    ap.add_argument('--hot', action='append', default=[], help='mapping: PLACE=NAME, a hotkey on that button')
    ap.add_argument('--readonly', action='store_true', help='mapping: a layout set in the launcher')
    ap.add_argument('--open', action='store_true', help='the cursor in the page')
    ap.add_argument('--row', type=int, default=1, help="the page's cursor row")
    ap.add_argument('--scroll', type=int, default=0)
    ap.add_argument('--set', action='append', default=[], help='Section.Key=Value over the repo\'s GHR.ini')
    ap.add_argument('--menu', choices=['western', 'japanese', 'classic'], default='western')
    ap.add_argument('--prompts', choices=sorted(HINT_ART), default='xbox')
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
    page = {'controls': 0, 'display': 1, 'interface': 2, 'gameplay': 3, 'sound': 4, 'mapping': 0}.get(a.page, -1)
    env = dict(os.environ, KAMITE_PREVIEW_HINTS='|'.join(hint_paths(work, a.menu, a.prompts)),
               KAMITE_PREVIEW_TITLES=title_letters())
    menu = {'western': 'western', 'japanese': 'japanese', 'classic': 'classic'}[a.menu]
    a.set.insert(0, 'Game.MenuButtons=' + menu)  # the page's menu words follow it (a --set wins)
    if a.page == 'mapping':
        folder = {'xbox': 'xbox360', 'playstation': 'ps3', 'nintendo': 'switch', 'steam': 'steamdeck', 'keys': 'xbox360'}[a.prompts]
        pad = {'ps3': 'playstation', 'switch': 'nintendo'}.get(folder, 'xbox')
        art_dir = os.path.join(work, 'resources/options/mapping')
        os.makedirs(art_dir, exist_ok=True)
        src = os.path.join(ROOT, 'resources/options/mapping')
        for name in os.listdir(src):
            if name.endswith('.txt'):
                open(os.path.join(art_dir, name), 'w').write(open(os.path.join(src, name)).read())
            elif name.endswith('.png'):
                im = Image.open(os.path.join(src, name)).convert('RGBA')
                open(os.path.join(art_dir, name) + '.rgba', 'wb').write(struct.pack('<II', *im.size) + im.tobytes())
        icons = os.path.join(work, 'resources/prompts/xelu', folder)
        os.makedirs(icons, exist_ok=True)
        for name in os.listdir(os.path.join(ROOT, 'resources/prompts/xelu', folder)):
            im = Image.open(os.path.join(ROOT, 'resources/prompts/xelu', folder, name)).convert('RGBA')
            open(os.path.join(icons, name) + '.rgba', 'wb').write(struct.pack('<II', *im.size) + im.tobytes())
        env['KAMITE_PREVIEW_MAPPING'] = '|'.join([work, os.path.join(art_dir, 'pad_%s.txt' % pad), folder, a.line,
                                                  '1' if a.readonly else '0', ','.join(a.swap), ';'.join(a.hot)])
    subprocess.check_call([exe, raw_in, str(w), str(h), raw_out, '0' if a.title else '1', str(a.cursor), str(a.confirm),
                           os.path.join(fonts, 'ArchivoNarrow-SemiBold.ttf'),
                           os.path.join(fonts, 'DejaVuSans-Bold.ttf'), title] + bars[:2] +
                          [str(page), '1' if a.open else '0', str(a.row), str(a.scroll), bars[2],
                           os.path.join(ROOT, 'GHR.ini')] + a.set, env=env)
    Image.frombytes('RGB', (w, h), open(raw_out, 'rb').read()).save(a.out)

if __name__ == '__main__':
    main()
