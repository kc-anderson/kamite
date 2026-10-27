Motions the NPCs tab (F2) can give a bystander (the file's name is the motion's id in npc_poses.txt):

- counter_idle.mot: hands on a low counter. K's own animation (ours to ship, no credit needed), made with
  tools/models/npc_motion.py (pelvis travel 40%, the head's turn taken out), then calmed and slowed with
  tools/models/mot_rework.py (--slow 1.4 --calm 0.4 --sway 0.02 --sway-roll 2).
- counter_tease.mot: bent over a counter, the hips swaying (slowed 1.5x, smoothed). K's own animation.
- dance.mot: Quaternius' Universal Animation Library 1, Dance_Loop (CC0), by tools/models/ual_motion.py.
- idle_fold_arms.mot: Quaternius' Universal Animation Library 2, Idle_FoldArms_Loop (CC0), by ual_motion.py.
- female_idle_1.mot, female_idle_2.mot: Denys Almaral's Free Animation Pack for City People (female_idle1_150f,
  female_idle2_190f), by tools/models/npc_motion.py (rig biped, --slow 1.0). See docs/licenses.

The UAL and biped motions take the upper body and the hips from the animation; the legs are the bunny girl's idle on
the game's leg IK, so the feet stay planted. Old names still load (npc_poses.cpp kRenamed): table_tease is
counter_tease; counter_idle_calm_slow, counter_idle_calm and counter_idle_slow are counter_idle.

A file of the same name in mods/npc_motions replaces one of these.
