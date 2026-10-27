"""Build the playable character mods from your own disc: assets/characters/<id>/ -> <out>/mods/characters/<id>/.

    python -I tools/models/build_characters.py --iso <disc image .iso> --out <folder holding kamite.exe> [--texture-pack <dir>] [id ...]

Each assets/characters/<id>/character.json lists the disc files it replaces:
    {"files": {"pl/pl00.dat": {"nodes": "xavier_nodes.scr", "textures": "xavier_textures.tm3"},
               "pl/pl01.dat": {"nodes": "xavier_coat.scr", "textures": "xavier_textures.tm3", "keep": [],
                               "append_textures": false}}}
"nodes"/"textures": the mod's own model nodes and textures merged into the game's file (its skeleton, motions and the
"keep" nodes, by default the gauntlet arm, stay the game's; textures replace the game's of the same name, new names
are appended unless "append_textures" is false); "trim": {"GH_00": {"axis": [-1, 0, 0], "min": 0.27}} cuts a kept
node at the plane axis.p = min (rest pose, model space), keeping the far side and clipping the triangles that cross it
(e.g. the gauntlet arm where it enters the character's own upper arm); a "tint": {"start": s, "length": l, "rgb": [r,
g, b]} on a trim multiplies the node's vertex colours by rgb/128 up to axis.p = s, fading to unchanged over l; an
"inset": {"start": s, "factor": f, "centre": [cy, cz]} pulls the vertices between the cut and axis.p = s towards the
bone line (by f at the cut, unchanged at s), so the part inside the character's own stump stays hidden; "empty": every
model node of that file drawn as nothing; "overlay": {"<game texture>": "<RGBA png in the character's folder>"} lays a
hand-painted layer over one of the game's own textures (Xavier's fur over Gene's arm skin): the 16-colour texture in the
file is requantized with it on, and its full-resolution replacement (the pack's image under --texture-pack, else the
game's texture scaled up) gets it at full size in the character's textures folder. Overlays need numpy and Pillow. A "textures" folder in the
character's assets (PCSX2 replacement images named by the in-game textures' hashes, made by the character's encoder) is
copied to <out>/mods/characters/<id>/textures: the runtime adds it to the replacement folders, so the character's
textures draw at full resolution. The output is built
from your disc, so like the rest of the build it stays private. Standard library only, except overlays.
"""
import json
import os
import shutil
import struct
import sys

texture_pack = None  # --texture-pack <folder holding replacements/>: the HD pack the overlays go on

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "xavier"))
sys.path.insert(0, os.path.join(ROOT, "tools", "snr2"))
import gamemodel  # noqa: E402
from snr2disc import Disc  # noqa: E402


def build(iso, out, only=()):
    disc = Disc(iso)
    paths = {p.lower(): i for p, i in disc.fst_files()}
    base = os.path.join(ROOT, "assets", "characters")
    built = 0
    for cid in sorted(os.listdir(base)):
        folder = os.path.join(base, cid)
        spec_path = os.path.join(folder, "character.json")
        if (only and cid not in only) or not os.path.isfile(spec_path):
            continue
        spec = json.load(open(spec_path, encoding="utf-8"))
        dest = os.path.join(out, "mods", "characters", cid)
        os.makedirs(dest, exist_ok=True)
        for rel, how in spec["files"].items():
            if rel.lower() not in paths:
                raise SystemExit("%s: %s is not on the disc" % (cid, rel))
            game = disc.read_afs(paths[rel.lower()])
            data, layers = merge_file(game, folder, how)
            if layers:
                overlay_hd(dest, game, data, layers, texture_pack)
            if len(data) > len(game):
                print("  %s/%s is %d bytes, the game's %d (the port grows the archive and the player's buffer)" % (cid, rel, len(data), len(game)))
            target = os.path.join(dest, *rel.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(data)
            print("  %s: %s %d -> %d bytes" % (cid, rel, len(game), len(data)))
        hd = os.path.join(folder, "textures")
        if os.path.isdir(hd):
            target = os.path.join(dest, "textures")
            if os.path.isdir(target):
                shutil.rmtree(target)
            shutil.copytree(hd, target)
            print("  %s: %d full-resolution textures" % (cid, len(os.listdir(target))))
        if os.path.isfile(os.path.join(folder, "character.ini")):
            shutil.copyfile(os.path.join(folder, "character.ini"), os.path.join(dest, "character.ini"))
        built += 1
    disc.close()
    print("%d character(s) built into %s" % (built, os.path.join(out, "mods", "characters")))
    return 0


def merge_file(game, folder, how, nodes=None, textures=None):
    """The game's file merged with the character's, as character.json's entry 'how' says. nodes / textures: the
    mod's .scr / .tm3 bytes instead of the files named in 'how' (encoders check a fresh encode with this). Returns
    (data, the overlay layers {game texture name: RGBA array})."""
    if how.get("empty"):
        return gamemodel.empty_model(game), {}
    if nodes is None:
        nodes = open(os.path.join(folder, how["nodes"]), "rb").read()
    if textures is None:
        textures = open(os.path.join(folder, how["textures"]), "rb").read()
    trim = {}
    for name, t in how.get("trim", {}).items():
        tint, inset = t.get("tint"), t.get("inset")
        trim[name] = (tuple(t["axis"]), t["min"],
                      (tint["start"], tint["length"], tuple(tint["rgb"])) if tint else None,
                      (inset["start"], inset["factor"], tuple(inset["centre"])) if inset else None)
    layers, replace = {}, {}
    if how.get("overlay"):
        import numpy as np
        from PIL import Image
        import paint
        layers = {name: np.asarray(Image.open(os.path.join(folder, png)).convert("RGBA")) for name, png in how["overlay"].items()}
        game_texs = [(nm, tb) for t, d, o in gamemodel.dat_read(game) if t == "TM3" and d is not None for nm, tb, _ in gamemodel.tm3_read(d)]
        replace = paint.overlay_textures(game_texs, layers)
    data = gamemodel.merge_model(game, nodes, textures, tuple(how.get("keep", gamemodel.KEEP)),
                                 how.get("append_textures", True), trim, replace)
    return data, layers


def overlay_hd(dest, game, merged, layers, texture_pack):
    """Full-resolution replacements for the overlaid textures: the pack's replacement of the game's texture (an
    uncompressed BGRA .dds, as texture packs for the game ship) or the game's own texture scaled up 4 times, with the
    layer laid on at that size, saved under the new texture's replacement name."""
    import numpy as np
    from PIL import Image
    import paint
    from gametex import tim3_decode
    from pcsx2tex import replacement_name
    game_tex = {nm: tb for t, d, o in gamemodel.dat_read(game) if t == "TM3" and d is not None for nm, tb, _ in gamemodel.tm3_read(d)}
    new_tex = {nm: tb for t, d, o in gamemodel.dat_read(merged) if t == "TM3" and d is not None for nm, tb, _ in gamemodel.tm3_read(d)}
    hd_dir = os.path.join(dest, "textures")
    os.makedirs(hd_dir, exist_ok=True)
    for name, layer in layers.items():
        if name not in game_tex or name not in new_tex:
            continue
        base, source = None, "the game's, scaled up"
        if texture_pack:
            dds = os.path.join(texture_pack, "replacements", replacement_name(game_tex[name]) + ".dds")
            if os.path.isfile(dds):
                b = open(dds, "rb").read()
                dh, dw = struct.unpack_from("<II", b, 12)
                if struct.unpack_from("<I", b, 88)[0] == 32 and len(b) >= 128 + dw * dh * 4:
                    px = np.frombuffer(b[128:128 + dw * dh * 4], np.uint8).reshape(dh, dw, 4)
                    base, source = np.ascontiguousarray(px[..., [2, 1, 0, 3]]), "the pack's"
        if base is None:
            rgba, info = tim3_decode(game_tex[name])
            base = np.asarray(Image.fromarray(rgba, "RGBA").resize((info["w"] * 4, info["h"] * 4), Image.LANCZOS))
        out = paint.overlay(base, layer)
        Image.fromarray(out[..., :3], "RGB").save(os.path.join(hd_dir, replacement_name(new_tex[name]) + ".png"), optimize=True)
        print("  %s: %s overlaid on %s (%dx%d)" % (os.path.basename(dest), name, source, base.shape[1], base.shape[0]))


def main(argv):
    args = list(argv[1:])
    def opt(name):
        if name in args:
            i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
        return None
    iso, out, pack = opt("--iso"), opt("--out"), opt("--texture-pack")
    if not iso or not out:
        print(__doc__)
        return 2
    global texture_pack
    texture_pack = pack
    return build(iso, out, tuple(args))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
