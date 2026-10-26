# Building and using Kamite

How to build Kamite from source, play it, and work on it. The [README](../README.md) has the overview.

## How to set it up

### What you need

1. **Windows 10 or 11** and a reasonably modern graphics card.
2. **Visual Studio 2022 Build Tools** with "Desktop development with C++" (free from Microsoft).
3. **Python 3.10 or newer**.
4. **Your own disc image of the game** (USA, `.iso`, serial SLUS-21503). It must be this exact version: check it against
   [DISC_HASHES.md](../DISC_HASHES.md). The port refuses any other disc.

### Build it

Open a terminal (PowerShell) and run these one at a time:

```powershell
git clone https://github.com/kris-c-anderson/kamite.git
cd kamite
copy "D:\path\to\your-disc.iso" game\
python -I build.py generate
python -I tools\snr2\snr2.py --iso "game\your-disc.iso" extract
python -I tools\snr2\gen_modules.py
python -I build.py modules
python -I build.py vu
python -I build.py configure -DKAMITE_PCSX2_GS=ON -DKAMITE_SDL_INPUT=ON
python -I build.py build
python -I build.py characters
```

- `generate` and the SNR2 steps turn the code from your disc into C++ and take a while; `vu` compiles the PS2's VU1
  microcode ahead of time; `characters` builds the playable characters from your disc.
- `build` downloads a few libraries the first time and then compiles everything (expect 10+ minutes).

### Play

Double-click **`Start-Kamite.cmd`**, or run `build\port\bin\kamite.exe`. The launcher opens first: point it at your
disc image once, then Play. The very first start can sit on a black screen for a couple of minutes while the graphics
are prepared for your card; later starts take seconds.

Your settings are in `build\port\bin\kamite.ini` (every launcher option is a line there, with a comment), your saves in
`build\port\bin\memcards\`, your controls in `build\port\bin\controls.ini`.

### Options

| Where | What |
|---|---|
| Display | Aspect ratio, HUD layout (16:9 edges or 4:3), menu layout (4:3 or stretched), camera (presets; with "Show advanced settings": field of view, in fights, distance, tilt, enemies following the view), see-through walls, window / full screen, window size |
| Graphics | Internal resolution (1x–8x), post effects (PS2 resolution or upscaled), HD texture pack and its folder |
| Gameplay | 60 FPS, L3 dash, control style (classic / modern with mouse camera, work in progress), roulette timer (normal / double), Unlock Hard, mute music |
| Mods | Character: Gene or Xavier |
| Controls | Every key, mouse button and controller button (also F1 in game) |

In game: **F1** controls, **F2** in-game menu (the camera, tuned live), **F6** / **F7** menu and HUD layout, **F11** full screen, **F12** debugger.

---

## Mods

### Playable characters

A character is a folder `mods/characters/<id>/` next to the game with the disc files it replaces (`pl/pl00.dat` is
Gene's model) and, optionally, `textures/` with full-resolution replacements in PCSX2's format. The port writes them
over the game's files as the disc is read, so the game itself is never patched. Xavier is the example mod:
`assets/characters/xavier/` is its source; `tools/models/` has the model and texture writers for the game's formats and the Blender pipeline
that put Xavier onto Gene's skeleton (`tools/models/xavier/README.md`).

### HD textures

Any PCSX2 texture pack for the game (USA, SLUS-21503) works: set its folder in Graphics, or put it at `textures/SLUS-21503/`
next to `kamite.exe`.

---

## Troubleshooting

| Problem | What to do |
|---------|------------|
| "Wrong disc" message | Your image is a different version. Compare it with [DISC_HASHES.md](../DISC_HASHES.md). |
| Black screen on the very first start | Wait: the graphics are being prepared. Later starts are fast. |
| Short pauses the first time an effect appears | Its shaders are being prepared; they do not come back once seen. |
| Controls screen (F1) does not draw | With the PCSX2 renderer it cannot be drawn yet: use the launcher's Controls tab. |
| The game crashes or freezes | Please report it with the log (next section). |

## Reporting bugs

Open an issue and attach the log of your last session: `build\port\bin\logs\play_last.log` (the session before that is
kept as `logs\play_<time>.log`), and `logs\crash.txt` if there is one. Say where in the game it happened and which
options were on (60 FPS, aspect ratio, character).

---

## For developers

- **How it works:** the main program (`SLUS_215.03`) is recompiled to C++ with the recompiler in `runtime/ps2xRecomp`;
  the 81 code overlays the game loads (rooms, events, memory card) are relocated and recompiled by `tools/snr2`, and
  `src/kamite` installs the right one as the game loads it. The PS2 hardware (graphics, sound, I/O processor) is in
  `runtime/`; graphics go through PCSX2's renderer (`thirdparty/pcsx2-gs`). Every game-specific fix is commented where
  it is made.
- **Function names:** `recomp/kamite_functions.csv` carries the boundaries and names of the game's functions; the names
  come from the [decomp](https://github.com/LucasPicoli/god-hand-decomp) and are refreshed with
  `python -I tools/symbols/import_decomp_names.py <decomp checkout>`. The generated code, the crash log, the hang dump,
  the PC sampler and the call profiler all use them.
- **Switchable patches:** `[[patches.conditional]]` and `[[patches.hooks]]` in `recomp/kamite.toml` (and per module in
  `recomp/modules/`) are compiled in both forms and picked by a run-time flag; `src/kamite/frame_patches.cpp` sets the
  flags from `kamite.ini`.
- **Faster 3D:** `python -I build.py vu` compiles every VU1 microcode upload found on the disc. A few uploads are placed
  by the game itself: play with `PS2X_VU1_CAPTURE=<folder>` set, then `python -I build.py vu <folder>` and build again.
- **Diagnostics** (environment or `[Runtime]` in `kamite.ini`): `KAMITE_PC_SAMPLE` (`KAMITE_PC_SAMPLE_ALL=1` adds
  every thread of the process: busy share and hottest host functions, for a frame's bottleneck outside the EE),
  `KAMITE_CALL_PROFILE`, `KAMITE_FIND_COUNTERS`, `KAMITE_TRACE`; F8 in game dumps RAM and the VU1 trace; F6 writes one
  frame's 2D primitives and every drawn layout's entry table to the log.
- **Watching upstream:** changes in LasVola's recomp and in the decomp are reviewed weekly and ported when they help.
