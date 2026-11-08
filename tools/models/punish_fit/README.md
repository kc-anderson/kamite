# Punish fit: the punishes rebuilt offline

Gene and a victim playing a paired punish frame by frame, as the game does (claude/punish-notes.md "Punish fit"):
both halves from em/em00.dat, the placement of func_001299F0 (scene.align), root motion from channel 255 (scene.Track),
legs by IK to the motions' foot targets (bones 20 / 24 translation, fk.legik), meshes skinned (mesh.py) and drawn
flat-shaded (raster.py). contacts.py finds where Gene touches the victim; report1.py renders the four originals.

    GODHAND_EXTRACT=<disc extract> python -I tools/models/punish_fit/report1.py Stinger Cobra Spanking Suplex

Needs numpy, scipy and Pillow. What it draws is the game's art: keep the output private.
