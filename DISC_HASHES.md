# The disc image Kamite expects (verified 2026-10-17)

The game: NTSC-U (USA), serial SLUS-21503. SYSTEM.CNF says `VER = 1.00`, but that is only the version label on the disc. The hashes below identify the exact image.

| File | Size (bytes) | SHA-256 | SHA-1 | MD5 |
|---|---|---|---|---|
| The USA disc image (`.iso`) | 1,567,686,656 | `268B8E284619BB9E07D5C522D7E7CFA14CB0498623D7FBA04A97D18B63BC0A7C` | `2215FB65A9AE391741950970BD88B79CA87D9D05` | `EDFE683B6973FE45CBD75E30664F2A70` |
| `SLUS_215.03` (boot ELF on the disc) | 3,607,008 | `1742F95BEF65BDB2AA57B7A77DF4AC7619A092E9646E1EA325BC32EC8A64F3CD` | – | – |
| The `.7z` archive the image came from | 1,003,484,472 | `8E7981F415CF0D3E94872883C65A5F2BBC48E2F17E56311E6A130C97989E986D` | `79147AE65FB02C3A0ED89C4670CFD10D283B5CA4` | `7FFDBA5F7D717D8280C937B1F657ABA2` |

- The boot ELF SHA-256 equals the `elf_sha256` target of LucasPicoli/god-hand-decomp (`config/versions.json`).
- The port checks this ELF hash at startup (`src/kamite/game_disc.cpp`) and refuses any other disc.
- Put the ISO in `game\` (or pass `--iso` / set `KAMITE_ISO`).
- Recompute (PowerShell): `Get-FileHash "<file>" -Algorithm SHA256` (also SHA1 or MD5).
