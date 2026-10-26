# Kamite and the decomp

Kamite uses the matching decompilation (LucasPicoli/god-hand-decomp) and does not contribute code to it.

- **We use their work.** Function names come from their `config/symbol_addrs.txt` (`tools/symbols/import_decomp_names.py` → `recomp/kamite_functions.csv`), and struct offsets go under their field names (`src/kamite/game_layout.h`). The weekly upstream watch picks up new names.
- **We report what's wrong.** When port work shows a mistake in the decomp, it becomes a short issue there. Only corrections, no "names we found" and no pull requests. K posts the issues himself, with no tool attribution, which is their rule.
- **We keep a log.** What the port learns about the game goes in `findings.md` as a side effect of port work, ready if someone there asks. No separate research and no decomp builds on our side.

| File | What |
|---|---|
| `issue-camera-eye-target.md` | cCamera's `eye` (0x200) and `target` (0x210) are swapped (sceVu0CameraMatrix's arguments). To post. |
| `issue-player-mode5.md` | `UpdateBossPhase_10B780` is entry 5 of the player's mode table, not a boss routine. To post. |
| `findings.md` | What Kamite knows that the decomp doesn't name yet, with how sure we are. |

To post an issue, go to github.com/LucasPicoli/god-hand-decomp → Issues → New issue, and paste the title and body from the file after its "Before you post it" checks. No setup or build is needed.
