# OPTIONS screen

`wall.jpg`: the backdrop of the OPTIONS screen, K's own painted brick wall (no credit). 32:9: his 16:9 wall in the
middle, widened with his blank version of it by `tools/options/compose_wall.py`, then graded to the game's own Options wall
(brightness, contrast, saturation) by `tools/options/grade_wall.py` so the labels read as on the PS2. The game crops it to the picture's
shape and turns it into the backdrop texture's full-resolution image (`src/kamite/options_screen.cpp`).

`options_title.png`: the screen's OPTIONS title, K's own painted lettering (no credit), drawn by the renderer's overlay where the
game's painted title sat (`src/kamite/options_overlay.cpp`).
