# Issue: `UpdateBossPhase_10B780` is the player's mode-5 step, not a boss phase

**Upstream:** LucasPicoli/god-hand-decomp, as an issue (their rule: a placeholder is replaced only by a recovered name, so a better placeholder is the maintainers' call).

## Title

```
UpdateBossPhase_10B780 is entry 5 of the player's mode table D_003BD0B8
```

## Body

```
UpdateBossPhase_10B780 (src/cod/00107ec8.c) is not a boss routine, as far
as I can tell: it is the step function for mode 5 of the player object.

The dispatcher in func_00108AB0 (at 0x001092FC) indexes the table at D_003BD0B8
with the player's mode byte (+0x2F4): lbu 0x2F4($s1), sll 3, then a
pointer-to-member call through the 8-byte entry. The table has 12 entries
(0xFFFF0000, handler) and entry 5 is 0x0010B780:

  0 0x10B6B0  1 0x11A820  2 0x122C48  3 0x10BE10  4 0x10B760  5 0x10B780
  6 0x10B9C0  7 0x10BE18  8 0x10C300  9 0x10C5A8 10 0x10C930 11 0x10CB10

The body fits that: it turns the player's solid collision off, plays a
motion from the player's motion data (base +0x28 / +0x2C) on a small
sub-state at +0x2F6 and loops it, and sets bits 0x404 in +0x3AC. Running
the game, the player sits in mode 5 while a sub screen with a move preview
is open (the Techniques screen), so a name along the lines of
PlayerMode5_step_10B780 (or whatever you prefer) would be closer.

Entries 1, 2 and 4 of the same table currently carry the names
cOmPhased_resetAndRunPhase, cEm00_stepByPhaseTable and
Tramp_func_0014EA50_0010B760; I haven't checked those bodies, but being
player mode handlers they may be worth a second look too.
```

## Before you post it

- Read the dispatcher yourself: `asm/cod/000000.s` around `0x001092FC` (search `D_003BD0B8`).
- The table bytes come from the ELF: `.data` at 0x3BD0B8, 12 x 8 bytes. A one-liner prints them (`tools/` has nothing for it; any ELF reader works). Don't paste more of the data than the 12 addresses above.
- "Mode 5 while the Techniques screen is open" is what Kamite's style presets rely on (`src/kamite/style_presets.cpp`, `inPlay`: `+0x2F4 != 0` means not in play). If you'd rather not claim the Techniques part, drop that sentence; the table evidence stands on its own.
