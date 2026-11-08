# Punish fit: the punishes rebuilt offline

Gene and a victim playing a paired punish frame by frame, as the game does (claude/punish-notes.md "Punish fit"):
both halves from em/em00.dat, the placement of func_001299F0 (scene.align), root motion from channel 255 (scene.Track),
legs by IK to the motions' foot targets (bones 20 / 24 translation, fk.legik), meshes skinned (mesh.py) and drawn
flat-shaded (raster.py). contacts.py finds where Gene touches the victim; report1.py renders the four originals.

    GODHAND_EXTRACT=<disc extract> python -I tools/models/punish_fit/report1.py Stinger Cobra Spanking Suplex

Needs numpy, scipy and Pillow. What it draws is the game's art: keep the output private.

fit3.py fits a body to the original's touch points: per stage, keys every 4 frames of Gene's placement (forward,
side, turn), Gene's and the victim's hip height (the legs bend by the IK, feet stay put) and the victim's waist bend,
with smoothness, loop and stage-continuity terms. compare.py draws original / no fit / placement only / full fit.

Fitting every body (Nov 8): fit_all.py runs fit3 for every skeleton group of bodies.json (humanoid skeletons of
em00..em7f, by bones 0..29) and the four punishes into fits/; emit.py writes resources/punish_fit.txt (one fit block per
runtime body key, verdict by the touch error: off when 12 cm or more); review2.py <punish> <group> <file> <tag>
draws a review card (original / no fit / fitted, and a GIF of the fitted pair).

Gene's pose (Nov 8, step 3): fit5.py adds Gene's torso (bone 1 pre-rotated in bone 0's space: x, z) and arms (upper
arm 13 / 7 in the clavicle's space: raise about z, swing about y; forearm 14 / 8 about y) and the victim's size
(0.95..1.05) to fit3's values, as the runtime applies them (mot_edit preRotate). fit7.py solves it in two steps: A
(placement, heights, bend, size; Gene's torso pushed out of the victim) and a per-key refine of the 8 pose angles
against a signed-distance grid of the victim (libigl: pip install libigl) over every frame the key shapes: Gene's
points no deeper in the victim than the original pair at that frame + 1 cm, the arm touches kept. fit_pose.py runs
it per punish x body type (the type's representative) at enemy sizes 1.0 and 1.3 into fits7/, then every other body
of the type with the representative's pose held (--fix: bodies still past 2.5 cm get their own pose). emit7.py
writes resources/punish_fit.txt; review3.py + page3.py the review page (site3/).

gorilla_elbows.py (K, Nov 8): the Gorilla Suplex on the fat men with Gene's original arms; the victim raised per key so
his elbows sit at Gene's (to frame 34, eased out by 46), Gene's arm touches on the victim's shoulders / upper arms moved
to his elbows, placement and the rest fitted with that height held (fit5 P['fixvh']). emit7 also writes the body
lines (skeleton -> type) the enemy labels show.

spank_fat2.py (K, Nov 8): Spanking on the fat men keeps the game's placement; Gene's holding (left) arm gets one raise /
swing / elbow per body, held through the hold and the spanks (eased in over the grab's first 8 frames, out from frame
12 of the finish to the launch), fitted against the victim's mesh (1 cm) and the hand's touch spots on the back. A
per-key fit (spank_fat.py) made the arm flail.

fits7/ holds every fit7 result (rep_<punish>_<type>_<size>.npy / .json = the type's representative, g_<punish>_<group>_
<size> = the other bodies; X = keys x 14 values, json = metrics, keys, size=). emit7.py rebuilds resources/
punish_fit.txt from it: change one fit by re-running its job (fit_pose rep_job / group_job / fix_job, gorilla_elbows,
spank_fat2) and emitting again. find_model_field.py probes a case's RAM for the enemy's model-file field (not found yet).
