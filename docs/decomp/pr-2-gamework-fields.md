# PR 2: GameWork's pad words, frame divider and KeyStop bit

**Upstream:** LucasPicoli/god-hand-decomp, against `main` at beb2575f (independent of PR 1: either can land first).
**Patch:** `patches/pr-2-gamework-fields.patch`.
**Kind:** header field names only (`include/godhand/gameWork.h` and the boot TU's local view of the same block). Byte-identical by construction (field names don't reach the object code); checked with the full build.

## Commit message

```
headers: name GameWork's pad words and frame divider

GameWork 0x00-0x0C are the four pad words func_0031B560 fills each
frame: the buttons held, last frame's, pressed ((held ^ old) & held,
the existing padBtn) and released ((held ^ old) & old). 0x569 is the
number of vblanks a game frame waits for: the main loop in func_0030AC40
spins until the vblank count D_00753D80 reaches it (2 at boot, set by
func_0030B298; 1 runs the game at 60 frames a second). KeyStop sets
bit 0x02000000 of flags5D8.
```

## The change, line by line

`include/godhand/gameWork.h`:

| Was | Now | Evidence |
|---|---|---|
| `char pad000[0x08];` | `int padHold;` (0x00), `int padOld;` (0x04) | func_0031B560 (called from func_0031A808 with `D_007474A0`): `lw a0,0x0(s0); sw a0,0x4(s0)` at 0x31B5BC..D0 copies the held word to 0x04 before the new reading is built in 0x00 (stick directions are folded in as bits, e.g. `| 0x20000` when the stick byte at +0x73 is below -0x50, 0x31B86C..80). |
| `int padBtn; /* 0x08 face-button bits, 0x10-0x80 */` | same field, comment: pressed this frame | 0x31B884..A4: `a2 = +0x00, v1 = +0x04, v0 = a2 ^ v1; +0x0C = v0 & v1; +0x08 = v0 & a2`. So 0x08 is every button that went down this frame (the face buttons are 0x10-0x80 of it, which is what the old comment saw). |
| `char pad00C[0x1A0 - 0x0C];` | `int padRel;` (0x0C) + `char pad010[0x1A0 - 0x10];` | same lines: released = changed & old. |
| `unsigned char unk569; /* 0x569 */` | `unsigned char frameVblanks; /* 0x569 ... */` | func_0030AC40 at 0x30ADD8..EC: `lbu v1,0x569(s0); lw v0,D_00753D80; sltu v0,v0,v1; bnez v0, loop` (spins on func_002B4BC0 until the vblank count reaches it). func_0030B298 sets it to 2 at boot (src/cod/0030b298.c, `g->unk569 = 2`). src/cod/002d9c10.c and 002d9ca8.c test `== 1` (60 frames a second) through raw offsets; left as they are. |
| (new) | `#define GAMEWORK_KEY_STOP 0x02000000 /* flags5D8: the controls are off (KeyStop) */` | KeyStop (0x31AFA0, a retail name): `D_00747A78 |= 0x2000000` then func_0031B410. |

`src/cod/0030b298.c` (the boot code's own `BootWork` view of the same block): `unk569` -> `frameVblanks`, and the one use `g->unk569 = 2` -> `g->frameVblanks = 2`.

Not included, on purpose: 0x18/0x1C (also written by func_0031B560, some kind of repeat or hold-timer masks; not worked out), and the other flags5D8 bits (func_001F6C40 sets six of them at once and their individual meanings are not proven).

How we know (outside the decomp): Kamite's 60 FPS option writes 1 to 0x747A09 (src/kamite/frame_patches.cpp) and the game runs at 60; Kamite's menu remapping reads 0x00/0x04/0x08 (src/kamite/menu_buttons.cpp). Not needed for review.

## Checks (run on beb2575f with the patch applied)

- `scripts/check_naming_sync.py`: passes (9460 carves, 0 mismatches).
- `compile.py`: pending (runs after PR 1's build).
- `scripts/session_check.sh`: pending.
- `git grep -n unk569 -- include src`: no matches left.
