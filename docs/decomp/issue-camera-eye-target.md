# Issue: cCamera `eye` (0x200) and `target` (0x210) look swapped

**Upstream:** LucasPicoli/god-hand-decomp, open as an issue (a header rename touches every camera body, so it is the maintainers' call how to land it).

## Title

```
cCamera: eye (0x200) and target (0x210) seem to be the other way round
```

## Body

```
include/godhand/cCamera.h names 0x200 `eye` and 0x210 `target`. The view
matrix code reads them the other way round, I think:

cCamera_calcViewMatrix (src/cod/00138648.c) passes `f.p`, built from
self->target (0x210), as the second argument of sceVu0CameraMatrix, and
`f.zd`, which is 0x200 minus 0x210, as the third. In the SDK's
sceVu0CameraMatrix(m, p, zd, yd), `p` is the camera's position and `zd`
the direction it looks in. So 0x210 is where the camera is and 0x200 is
the point it looks at, and zd = look-at point minus position, which is the
usual forward vector. With the current names the comment above the
function ("the view direction (eye minus target)") describes a vector
that points backwards.

Outside the decomp: in a PC port of the game I work on, a see-through
effect around the player uses 0x210 as the camera position (distance
from the camera to the player, projected with the view matrix) and it
lines up in game; using 0x200 puts it at the player's own position.

If that is right, swapping the two names (and the rot comment at 0x220,
"angles from eye to target") would be the fix. Happy to send the PR if
you want it, but it touches every camera body, so I'm asking first.
```

## Before you post it

- Open `src/cod/00138648.c` and check the lines yourself: `f.p` = `self->target`, `zd` = `f.e - target` where `f.e` is `self->eye` through `mtx[6]`.
- The see-through effect is `src/kamite/camera.cpp` (it reads `camera + 0x210` as the eye). Mention it only as you would any outside observation.
