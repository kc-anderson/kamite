Motions the NPCs tab (F2) can give a bystander, made with tools/models/npc_motion.py from K's own
animations (ours to ship, no credit needed):

- table_tease.mot: bent over a table, the hips swaying (slowed 1.5x, smoothed).
- counter_idle.mot: hands on a low counter (the counter idle; pelvis travel 40%, the head's turn taken out).
- To pick from (Oct 26; the one K picks becomes counter_idle.mot and the others go), made from counter_idle.mot with
  tools/models/mot_rework.py:
  - counter_idle_slow.mot: the same at 1/1.6 the speed (--slow 1.6).
  - counter_idle_calm.mot: the quick back-and-forth pump at 35% (--calm 0.35), with a slow weight shift side to side
    (--sway 0.02 --sway-roll 2, four times round the loop: 3.8 s each).
  - counter_idle_calm_slow.mot: both, milder (--slow 1.4 --calm 0.4, the same sway: 5.4 s each).

A file of the same name in mods/npc_motions replaces one of these.
