# Launcher images

`logo.png`: the KAMITE wordmark the launcher draws at the top of its Home and Install screens (any size; as wide as
fits, at most 44% of the window's height, trilinear filtered). It stays still. Without it the launcher writes the name
in text.

`kana.png`: the painted kana (with their shadow and splatter) drawn over the wordmark at `kKanaRect` (fractions of
logo.png, in src/kamite/launcher.cpp) and shifted a little with the mouse.

`burst.png`: the speed lines behind the logo (square, centred; turquoise with the line strength in alpha, fading to
nothing at the circle's edge). The launcher draws it twice, turning and pulsing, squashed to 3/4 height, centred on the
wordmark (`kLogoCentreX/Y` in src/kamite/launcher.cpp: 0.480, 0.737 of logo.png), and shifts it with the mouse.

`background.png`: the desert behind everything (K's own art, widened to 2600x941 with sand and sky taken from its own
empty left side). It fits the window's height up to 2.76:1 (wider windows crop it vertically), anchored right so the
town and the walkers stay in frame. `kBackgroundShader` in src/kamite/launcher.cpp adds heat haze along the horizon,
fake depth of field, procedural clouds (masked to blue sky pixels, so they pass behind the town) and a darkening
towards the bottom with a vignette so the text reads; its `kHorizon` (0.625) is the horizon's height in this image.
Without the file the launcher draws its brown gradient.

The logo images are built by `tools/logo/build_logo.py --wordmark <render.png> --kana <kana.png> [--psd layers.psd]`, or
from that PSD again with `--layers layers.psd`; the script prints the wordmark's centre for `kLogoCentreX/Y` and the
kana's rectangle for `kKanaRect` if the layout changes. The whole `resources/` folder is copied next
to GHR.exe at build time.
