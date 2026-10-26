# Findings not packaged yet

Things Kamite knows about the game that the decomp (at beb2575f) doesn't name, with how sure we are. Each line says what it would take to become a PR. Proven = read in the code; observed = seen working in the port, not traced in full.

## Ready for a small PR (proven, not packaged only to keep one change per PR)

- **cIDBaseEnt.b89 (0x89) = text alignment.** cIDBase_transText (0x2AB0B8) turns it into cMessDrawFont_setDrawPos's `alignFlags` (bit 0 centre X, bit 1 centre Y): 0 → 3 (centred both ways), 1..4 → 0 (top left), anything else → 1 (centred X). 0x2AB0D8..FC, the value goes to `$a1` at 0x2AB1E8. Name: `align`, with a comment giving the mapping. Header-only, like PR 2.
- **cMessDrawFont.unk2C[0] / unk2C[1] (0x2C / 0x30) = letter and line spacing.** The decomp's own bodies already use them that way: 002b0908.c adds `unk2C[0] * zoomX` after each character, 00172490.c adds `unk2C[1] * zoomY` per line. Name: `gap[2]` or `charGap` / `lineGap`. Header plus the five uses.
- **func_0030AC40 = the main loop.** Calls the frame's update and draw and waits for `frameVblanks` vblanks (see PR 2). Name only after PR 2 lands, so the evidence reads in their terms. A descriptive placeholder (`RunMainLoop_30AC40`).

## Observed in the port (needs the body read before a PR)

- **cIDBaseEnt.b8B (0x8B):** text and icon entries pass it to func_002AB9A8 (0x2AB0E0, 0x2AB348), which picks the colour set (set 1 draws black: the shadow entries use it); sprite entries use it as the texture number minus 1 (0x2AB3A8: `cIDManager_getTexAddr(..., b8A, b8B + 1)`). So the name depends on the kind; `texOrColorNo` or leave it.
- **cMessDrawFont.unk4C[0] (0x4C):** DrawMessageCode (0x2AFC48) skips inline icons when it is 3. Kamite saw it at 3 while the shadow copy of a line is drawn. Needs the writer found.
- **GameWork.flags5D8 bits 31 and 30:** the player's Move (func_00100530) returns at once on bit 30 (a player whose +0x2FE is not 0x101) and checks bit 31 for 0x101; func_001F6C40 sets bits 31..28, 19 and 18 together (the jukebox uses it to freeze the scene). Kamite clears 31/30 around Gene's Move while the jukebox is open and it works. Needs each bit's reader traced.
- **GameWork.flags5E4 & 0x40:** set during the casino slot machine and the dog race (Kamite's style presets stay off while it is set). Needs the setter found.
- **GameWork.language (0x56C) = 0 for the Japanese layout:** func_0031A808 swaps the confirm/cancel masks on it, func_002B12D8 the icon advance, func_002B1300 the icon's vertical offset, and the menu command table (0x3C3E50 vs 0x3C3D50) is chosen on it or debugFlags & 0x800. A comment line on the field ("0 = Japanese conventions: circle confirms") is the PR.
- **cPlCamera.unk51C (0x51C) > 0:** a battle or preset camera owns the view (Kamite's free camera backs off while it is set). The rest of what Kamite uses (0x250 fov, 0x410/0x414 pose.rotOfs as pitch/yaw, 0x420 pose.dist) the decomp already has.

## Functions with a likely role (placeholders, only after reading each body)

| Address | Role seen from the port |
|---|---|
| 0x151990 | TIM2 image loader (Kamite's texture hooks see every TIM2 pass through it). |
| 0x16C730 / 0x16DC18 / 0x16DF38 | The Techniques screen (cSubScr's screen at +0x310): update (re-reads cCoreSaveData.combo every frame), draw, input (never tests triangle). |
| 0x145238 / 0x145810 | The roulette (special-move wheel): dial draw and wheel slide. |
| 0x2A94F0 / 0x2A9620 / 0x2A9648 | A heap's reset, link and trim. |
| 0x107F58 | Player init (the coat is parented at 0x108068). |
| 0x307B28 | Per-frame hide-model manager (skipped while flags5D8 & 0x200000). |

## Bigger items (later, if they want them)

- **VU1 microprogram catalogue.** Which VU1 programs the game uploads, what each draws (scenery with the clipper, skinned models, the layout programs) and their entry points. Kamite's VU1 recompiler work produced this; it could be a doc in their `docs/`. Open an issue first (their rule for anything that isn't a match or a name).
