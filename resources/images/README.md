# Launcher images

The logo is five layers with the same canvas, drawn into one rectangle at the top of the Home and Install screens
(as wide as fits, at most half the window's height, trilinear filtered), in this order:

- `kana.png`: the flat turquoise kana behind everything, shifted a little with the mouse.
- `logo.png`: GOD HAND and its shadow. It stays still. Without it the launcher writes the name in text.
- `rearmed.png`: the REARMED plate without its letters.
- `rearmed_text.png`: the plate's letters, white with a slight shade; the launcher tints them gold.
- `rearmed_glow.png`: the letters blurred, white; the hot glow when Play is pressed (the plate then plays the game's
  roulette pick: the letters heat up and bloom, a yellow bar shoots out both ways and thins to streaks, while the
  rest fades to black).

They come from K's layered GHR_logo.psd through `tools/logo/export_launcher_logo.py --psd GHR_logo.psd --readme
assets/logo.png`, which also prints `kLogoCentreX/Y` (GOD HAND's centre) and `kRearmedRect` (the plate) for
src/kamite/launcher.cpp. GOD HAND itself is rendered by `tools/logo/render_wordmark.py` from K's letter SVG and his
hand-painted crack PSD; K's own effects pass is on top of that.

`burst.png`: the speed lines behind the logo (square, centred; turquoise with the line strength in alpha, fading to
nothing at the circle's edge). The launcher draws it twice, turning and pulsing, squashed to 3/4 height, centred on
GOD HAND (`kLogoCentreX/Y`), and shifts it with the mouse.

`background.png`: the desert behind everything (K's own art, widened to 2600x941 with sand and sky taken from its own
empty left side). It fits the window's height up to 2.76:1 (wider windows crop it vertically), anchored right so the
town and the walkers stay in frame. `kBackgroundShader` in src/kamite/launcher.cpp adds heat haze along the horizon,
fake depth of field, procedural clouds (masked to blue sky pixels, so they pass behind the town) and a darkening
towards the bottom with a vignette so the text reads; its `kHorizon` (0.625) is the horizon's height in this image.
Without the file the launcher draws its brown gradient.

`tools/logo/build_logo.py` built the old KAMITE logo (wordmark + kana over it); kept for its burst.png step. The whole
`resources/` folder is copied next to GHR.exe at build time.

`steam_logo.png`: the five logo layers flattened (REARMED's letters in the launcher's gold), 1280 px wide: the Steam
library logo Add to Steam writes (src/kamite/launcher_steam.cpp). Rebuild it the same way when the layers change.

`steam_icon.png`: src/kamite/ghr.ico's 256 px frame as a PNG, the Steam entry's icon (Steam reads the exe's
PNG-compressed icon frames with a black background).
