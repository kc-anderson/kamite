# Splash text

The launcher shows one line of `splash.txt`, picked at random each time it starts, tilted over the wordmark's bottom
right corner (Minecraft's splash text). One splash per line; empty lines are skipped; any length (long lines are drawn
smaller). Edit the copy next to kamite.exe (`resources/splash/splash.txt`) to try lines out; the repo's copy is synced
from K's own list.

`splash.fnt` + `splash.png`: Dominican by Harold Lohner (haroldsfonts.com) baked as a glyph atlas with
`tools/splash/bake_font.py` (free for any use; the raw .ttf is not redistributed, so it is not in the repo). Glyphs:
ASCII and the Latin-1 letters the font has; anything else draws as `?`.
