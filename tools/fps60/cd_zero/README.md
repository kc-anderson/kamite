# cd_zero: the countdown-hook census (Nov 7)

Sorts every 60 FPS countdown hook (km_fps60_countdown*, *_cd_*, countup) by what its code does when the count is at 0
on a second half (claude/fps60-notes.md "1 -> 0 step"). Run from this folder with SLUS=<path to SLUS_215.03>, in order:
census.py -> census2.py -> census3.py -> classify.py -> classify2.py -> conv2.py (each writes main*.pkl for the next).
Classes: LOAD-EXACT (load-raise, no test of the new value: countdownLoadExact), LOAD-KEEP (a test follows: old rule),
DEC-CONVERT / ELSE (hooked decrement behind a signed guard load: countdownGuard on the guard), DEC-POSTONLY / DEC-OK
(nothing to do), DEC-MIXED (a guard and a test of the new value: listed, unchanged). guards.py checks the guard loads.
