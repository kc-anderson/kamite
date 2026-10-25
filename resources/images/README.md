# Launcher images

`logo.png`: the KAMITE wordmark the launcher draws at the top of its Home and Install screens (any size; as wide as
fits, at most 44% of the window's height, trilinear filtered). It stays still. Without it the launcher writes the name
in text.

`kana.png`: the painted kana (with their shadow and splatter) drawn over the wordmark at `kKanaRect` (fractions of
logo.png, in src/kamite/launcher.cpp) and shifted a little with the mouse.

`burst.png`: the speed lines behind the logo (square, centred; turquoise with the line strength in alpha, fading to
nothing at the circle's edge). The launcher draws it twice, turning and pulsing, squashed to 3/4 height, centred on the
wordmark (`kLogoCentreX/Y` in src/kamite/launcher.cpp: 0.480, 0.737 of logo.png), and shifts it with the mouse.

All three are built by `tools/logo/build_logo.py --wordmark <render.png> --kana <kana.png> [--psd layers.psd]`, or
from that PSD again with `--layers layers.psd`; the script prints the wordmark's centre for `kLogoCentreX/Y` and the
kana's rectangle for `kKanaRect` if the layout changes. The whole `resources/` folder is copied next
to kamite.exe at build time.
