# Xavier

`assets/characters/xavier/` (the model nodes and textures) was made from `Xavier_v1_1_1-README.blend` (Xavier:
Renegade Angel, © Adult Swim) with these scripts: Blender 5.2 as a Python module (`pip install bpy==5.2.3 pillow
numpy scipy xxhash`). Environment: `XAVIER_BLEND`, `GENE_PL00` (the game's pl/pl00.dat, with pl01.dat beside it),
`XAVIER_WORK` (work folder), `XAVIER_PAINTED` (optional: hand-painted textures, see step 0). Every knob below is an
environment variable, for trying variants.

0. `unpack_textures.py`: the blend's packed images as PNGs into `<work>/textures`; files in `XAVIER_PAINTED` with
   the same names replace them (K's `XavierDiffuse.png` has the snake's eyes painted into their sockets, so the
   snake needs no eye geometry).
1. `prep1.py`: Xavier's mesh (shape keys applied, rest pose) on Gene's skeleton: scaled to Gene's height, legs and the
   snake arm laid along Gene's bones (hip-knee-ankle, so his knees bend forward with Gene's), the right arm cut halfway
   down the upper arm and the stump capped with fur (Gene's gauntlet arm comes out of it: `trim` in character.json),
   Xavier's deform bones mapped to Gene's 32. The boot rides the foot bone alone (`XAVIER_BOOT`); the jaw, lower lips
   and tongue ride Gene's jaw (bone 26); the mouth interior is kept on the face texture's bottom strip
   (`XAVIER_MOUTH`); the snake's jaw is marked with bone 31 and its hinge written to `snake_hinge.json`; the hair
   rides spare bones 29 (back) and 30 (side strands), whose pivots it prints (`[Hair]` for character.ini). Hair: the
   middle layer behind the ears is dropped (`XAVIER_HAIR_MID=1` keeps it), the hair below ear level is
   `XAVIER_HAIR_SHORTEN` (0.85) of its length all round; also `XAVIER_BUTT_SCALE`, `XAVIER_LOIN_OUT`,
   `XAVIER_HAIR_LIFT` / `_STAGGER` / `_FLARE`. Then `python -I face_mouth.py <work>/textures` (the mouth texture into
   the face's bottom strip, with the tooth strip painted over its old tooth shapes: rows 0-52 of the 512-row mouth
   texture, gum at the top).
2. `prep2.py`: the small parts rebuilt before decimation, so they keep their shape: each of the snake's 63 teeth
   becomes one flat two-sided triangle along its row (the fangs facing the head's sides), `XAVIER_SNAKE_TOOTH_WIDTH`
   (1.6) and `_LENGTH` (1.3) times the original cone; the snake's tongue is left out (it showed through the shut
   jaw). Xavier's own 30 teeth become two curved bands, the upper behind the upper lip and the lower below the lower
   lip, `XAVIER_TOOTH_SPAN` (4 cm) across the front of the arch, `XAVIER_TOOTH_SHIFT` (5 mm) towards the lips'
   opening and `XAVIER_TOOTH_FORWARD` (1 mm) forward in the middle (the model keeps its teeth hidden inside the lips),
   textured with the tooth strip face_mouth.py paints (`TOOTH_STRIP_PNG` for a hand-painted one). Then decimated to
   Gene's budget (the rest of the mouth interior to `XAVIER_MOUTH_KEEP`, 8%; the snake head at the body's ratio,
   `XAVIER_SNAKE_KEEP` 1), the body atlas split into UV-island groups; the loincloth panels are their own group.
3. `encode_xavier.py`: 4-bit 16-colour TIM3 textures in exactly the game's slots (Gene's names, sizes and order; the
   face turned 90 degrees, both eyes share pl0008), triangle strips (`strip.py`: grown both ways, best of 40 seeded
   tries), written as `xavier_nodes.scr`, `xavier_coat.scr` (the loincloth on Gene's coat-tail chains) and
   `xavier_textures.tm3`, plus `<work>/hd/`: each slot at full resolution under its PCSX2 replacement name. The snake's
   head goes into Gene's four left-hand meshes (LH_00 open, LH_01 the fist = shut, baked about the hinge), so the
   game swaps it as it swaps Gene's hand. It prints the merged pl00.dat's size (the build's own merge,
   build_characters.merge_file) against the game's player buffer (0x1E6800 bytes, heap A) and refuses more than
   `XAVIER_GROW_MAX` (64 KB) over it: the port grows the archive and the buffer for a larger file, out of heap A's
   free room (about 145 KB in a RAM dump of Oct 27; the game logs `character: pl/pl00.dat buffer ... largest free
   block` when it grows it).
4. `python -I requantize.py <folder>` with the encode copied into a folder (`xavier_*.scr`, `xavier_textures.tm3`,
   `hd/` as `textures/`): the 16-colour slots remade from the full-resolution images and renamed; then copy the
   folder into `assets/characters/xavier`.

`arm/pl0004_fur.png` and `arm/pl0004a_fur.png` are K's fur, painted where the gauntlet arm leaves the stump; the build
lays them over Gene's arm skin (character.json `overlay`). The game's skeleton, motions, gauntlet arm and the merge
into pl00.dat happen at build time from your disc (`tools/models/build_characters.py`), so nothing of the game is in
the repository.
