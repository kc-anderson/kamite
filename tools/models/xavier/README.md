# Xavier

`assets/characters/xavier/` (the model nodes and textures) was made from `Xavier_v1_1_1-README.blend` (Xavier:
Renegade Angel, © Adult Swim) with these scripts, Blender 5.2 as a Python module (`pip install bpy==5.2.3
pillow`), numpy and Pillow:

1. `prep1.py`: Xavier's mesh (shape keys applied, rest pose) moved onto Gene's skeleton: uniform scale to Gene's height
   (top of the head 1.837), legs and the snake arm aimed along Gene's bones, the right arm cut halfway down the upper
   arm (his shoulder and the top of the upper arm stay, skinned to Gene's arm bones, so the joint bends like his own
   skin; the game's gauntlet arm comes out of the cut, clipped at build time to start inside it: `trim` in
   character.json, the gauntlet a little further out than the arm's skin so it stays under the shoulder pad, and the
   skin's vertex colours tinted from the fur colour to plain over 8 cm: `tint`), the human mouth interior dropped,
   Xavier's deform bones mapped to Gene's 32 bones. The cut's open ring is capped with a disc of fur (nothing shows
   through the stump around the arm). Knobs, each also an environment variable for trying variants:
   `XAVIER_BUTT_SCALE` (0.96: the cheeks a little towards the pelvis, so they stay inside the loincloth),
   `XAVIER_HAIR_LIFT` / `XAVIER_HAIR_STAGGER` (3 mm + up to 4 mm: each hair strip pushed off the scalp and off the
   strips under it, against z-fighting), `XAVIER_HAIR_SHORTEN` (0.9: the hair below ear level 10% shorter),
   `XAVIER_HAIR_FLARE` (0.02 m: the lower hair pushed back from the neck).
   Oct 27: the boot is skinned to the foot bone alone (XAVIER_BOOT=0 for the old split, which let the upper trail the
   shin), the mouth interior is kept on the face texture's unused bottom strip (XAVIER_MOUTH=0 drops it as before) with
   its UVs squeezed there, the jaw, lower lips and tongue ride Gene's jaw bone 26 (his talking opens the mouth), and the
   snake's jaw and tongue ride the spare bone 31, which the port turns shut while Gene's fist mesh shows
   (bone_tweaks.cpp; prep1 prints the `[SnakeJaw]` hinge lines for character.ini). After prep1, run
   `python -I tools/models/xavier/face_mouth.py <work>/textures` (pastes the mouth texture into the strip).
2. `prep2.py`: decimated to Gene's budget (the mouth interior to 8% of its faces first), the body atlas split into UV-island groups (fur by colour, the rest by place);
   the two loincloth panels are their own group.
3. `encode_xavier.py`: 4-bit, 16-colour TIM3 textures in exactly the game's slots (same names, sizes and order as
   Gene's: the game's texture layout assumes them; the face is stored turned 90 degrees, both eyes share pl0008) and
   triangle-strip vertex buffers, written as
   `xavier_nodes.scr` (an MD with an empty bone table), `xavier_coat.scr` (the loincloth panels, weighted to the front
   and back bone chains of Gene's coat tails in pl/pl01.dat, so the game's cloth motion swings them) and
   `xavier_textures.tm3`, plus `textures/`: each slot again at full resolution (up to 1024x2048), named as PCSX2
   names a replacement for the in-game texture (`pcsx2tex.py`: XXH3 of the texture's GS memory blocks and CLUT;
   needs `pip install xxhash`). The runtime adds the character's textures folder to PCSX2's replacement folders, so
   the hardware renderer draws these instead of the 16-colour slots. Hair alpha: near-opaque texels are made exactly
   opaque (0x80), as in Gene's hair texture.

After encode: `python -I tools/models/xavier/requantize.py <out folder>` remakes the 16-colour slots from the
full-resolution images (and renames them to their new PCSX2 names) before copying into assets/characters/xavier.

Environment: `XAVIER_BLEND`, `GENE_PL00` (the game's pl/pl00.dat), `XAVIER_WORK` (work folder; Xavier's packed
textures saved as PNG in `<work>/textures`). The game's skeleton, motions, gauntlet arm and the merge into pl00.dat
happen at build time from your disc (`tools/models/build_characters.py`), so nothing of the game is in the repository.
