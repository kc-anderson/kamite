# PR 1: names for the message font's draw helpers

**Upstream:** LucasPicoli/god-hand-decomp, against `main` at beb2575f.
**Patch:** `patches/pr-1-message-font-names.patch` (no commit metadata; apply it and commit as yourself).
**Kind:** names only. No code changes, so the linked ELF stays byte-identical (see "Checks" below).

## Commit message

```
names: the message font's glyph, icon and colour helpers

Six func_ placeholders around cMessDrawFont_draw get descriptive names
(none of them are in the retail symbol table, so these are <Verb><Noun>
placeholders, not recovered names):

  func_002A52E8 -> SetFontColor_2A52E8
  func_002A53A8 -> DrawFontGlyph_2A53A8
  func_002AFC48 -> DrawMessageCode_2AFC48
  func_002B12A0 -> IsIconCode_2B12A0
  func_002B12D8 -> GetIconAdvance_2B12D8
  func_002B1300 -> DrawMessageIcon_2B1300

Updated in symbol_addrs.txt, carved_funcs and every TU that defines or
declares them.
```

The subject tag follows the repo's own naming commits (`names: ...`, e.g. beb2575f).

## What each function does (the evidence, so you can explain every line in review)

All six are already matched C in the decomp except `func_002A53A8`, so most of this can be read straight from their own sources.

| Address | New name | Evidence |
|---|---|---|
| 0x2A52E8 | `SetFontColor_2A52E8` | `src/cod/00148bd8.c`. Reserves 0x30 bytes in the font's packet buffer (`func_002A4F30(a0, 0x30)`) and writes one GIF tag with one A+D register (`0x1000000000008001`, regs `0xE`), data = the colour's four bytes as R, G, B, A plus Q = 1.0 in the high word, address 1 = **RGBAQ**. Callers pass the frame's colour, or `0xFF000000` (opaque black) for the drop shadow (`func_002AFC48`). |
| 0x2A53A8 | `DrawFontGlyph_2A53A8` | `asm/cod/000000.s`. Takes a 16-bit text code: `code >> 12` is the font page, compared with the loaded page at +0x205C and loaded through `func_002A4FD8` if it differs; `code & 0xFFF` the glyph. Reserves 0x90 bytes and builds a sprite packet (GIF tag `0x50000008`, `0x11000000`) for that glyph at x, y, z. Every caller draws a text character with it (`func_002AFC48`, the line drawer in `src/cod/002b0208.c`), z always 65535.0. |
| 0x2AFC48 | `DrawMessageCode_2AFC48` | `src/cod/00113768.c`. Draws one code of the message at x, y: sets the colour (`func_002A52E8`), then `func_002B12A0` decides icon or glyph: icons go to `func_002B1300`, characters to `func_002A53A8` (and twice more at (+1, +1) and (+1, -1) when the frame's flag bit 0 is set). With the font's flag bit 1 it only draws the black drop shadow at (+2, +2). It also steps the ruby record pointer when the body cursor reaches the record's anchor, and returns it. Called from `func_002AFC08` and from the character loop in `func_002AFED0` (`src/cod/002afed0.c`, which passes the `cMessDrawFont`). |
| 0x2B12A0 | `IsIconCode_2B12A0` | `src/cod/002b12a0.c`. Returns 1 for codes 0..13, else 0. Codes 0..13 are the inline button icons: every caller sends those to `func_002B1300` and the rest to the glyph drawer, and the width/height measures (`src/cod/002b0908.c`, `002b0a58.c`) use the icon advance for them. |
| 0x2B12D8 | `GetIconAdvance_2B12D8` | `src/cod/00150738.c`. Returns 26.0, or 32.0 when `D_00747A0C` (GameWork.language) is not 0: the horizontal advance of one inline icon. Used by the line-width and size measures (`src/cod/002b0908.c`, `002b0a58.c`). |
| 0x2B1300 | `DrawMessageIcon_2B1300` | `src/cod/002b1300.c`. Gets texture 0x18 of the system IDD (`cIDManager_getTexAddr_2ACE28(D_003C2388, 0x18)`: the button-icon sheet, 4 x 4 cells), picks the cell for the icon code (the switch on `*a0`: 0.25-unit UV squares), moves it 3 units down when the language is not 0, and draws it with `Gp_draw_polyFT4` at x, y and the given scale. |

Why not class method names: the retail symbol table names `cMessDrawFont_draw` (0x2AF728) and the setters around it but not these six, and the project's rule is "Never invent a class/method name the binary doesn't attest". So they get `<Verb><Noun>_<addr>` placeholders, like `ClearFields_13F2F8` in docs/STYLE.md.

How we know (outside the decomp): Kamite hooks `0x2AFB28..3C` (the icon advance inside `cMessDrawFont_draw`) and draws its own icons in place of codes 4..8 for the style list (src/kamite/style_presets.cpp); the button-prompt sheet patcher reads texture 0x18 (src/kamite/button_prompt_sheets.h). Both work in game, which is what pointed us at these functions. None of that is needed to review the PR: the evidence above is all in the decomp's own sources.

## Checks (all run on beb2575f with the patch applied)

- `scripts/check_naming_sync.py`: passes (9460 carves, 0 mismatches).
- `compile.py`: pending (full re-split and build running).
- `scripts/session_check.sh`: pending.
- `git grep -n 'func_002A52E8\|func_002A53A8\|func_002AFC48\|func_002B12A0\|func_002B12D8\|func_002B1300' -- src include config/symbol_addrs.txt compile_config.json`: no matches left.
