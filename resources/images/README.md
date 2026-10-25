# Launcher images

`logo.png`: the Kamite logo the launcher draws at the top of its Home and Install screens (any size; as wide as fits,
at most 44% of the window's height, trilinear filtered). Without it the launcher writes the name in text.

`burst.png`: the speed lines behind the logo (square, centred; turquoise with the line strength in alpha, fading to
nothing at the circle's edge). The launcher draws it twice, turning and pulsing, squashed to 3/4 height, centred on the
wordmark (`kLogoCentreX/Y` in src/kamite/launcher.cpp: 0.480, 0.737 of logo.png), and shifts it with the mouse.

Both are built by `tools/logo/build_logo.py --wordmark <render.png> --kana <kana.png> [--psd layers.psd]`, which also
prints the wordmark's centre for `kLogoCentreX/Y` if the layout changes. The whole `resources/` folder is copied next
to kamite.exe at build time.
