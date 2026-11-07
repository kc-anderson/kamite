# mtvu_trace.txt (PS2X_MTVU_TRACE=<seconds>) per displayed frame: VU1 job busy (S..E), GIF stage busy (g..G), the
# EE waiting on VU1 (W..w), jobs per frame, VU1 idle gaps. Codes: K kick, S/E VU job start/end, g/G GIF chunk,
# V vblank, D the game flips its display, R host present, W/w vu1Busy wait, X/Y scheduler events. Usage:
#   python -I tools/vu/mtvu_frames.py mtvu_trace.txt
import sys
ev = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) == 4: ev.append((int(p[0]), p[1], int(p[2], 16), p[3]))
ev.sort()
t0, t1 = ev[0][0], ev[-1][0]
print(f'trace {ev[0][0]/1e6:.2f}..{ev[-1][0]/1e6:.2f} s ({(t1-t0)/1e3:.0f} ms), {len(ev)} events')
flips = [e[0] for e in ev if e[1] == 'D']
vbl = [e[0] for e in ev if e[1] == 'V']
print(f'vblanks {len(vbl)} ({len(vbl)/((t1-t0)/1e6):.1f}/s), game flips {len(flips)} ({len(flips)/((t1-t0)/1e6):.1f}/s)')
def spans(start, end, thread=None):
    out = []; s = None
    for t, c, a, th in ev:
        if thread and th != thread: continue
        if c == start: s = t
        elif c == end and s is not None: out.append((s, t)); s = None
    return out
vu = spans('S', 'E', 'v'); gif = spans('g', 'G', 'g'); wait = spans('W', 'w', 'e')
def busy_in(sp, a, b):
    return sum(max(0, min(e, b) - max(s, a)) for s, e in sp)
rows = []
for a, b in zip(flips, flips[1:]):
    rows.append((b - a, busy_in(vu, a, b), busy_in(gif, a, b), busy_in(wait, a, b), sum(1 for s, e in vu if a <= s < b)))
import statistics as st
def q(v): v = sorted(v); return f'median {st.median(v)/1e3:.1f} ms, p90 {v[int(len(v)*0.9)]/1e3:.1f}, max {v[-1]/1e3:.1f}'
print('frame interval (flip to flip):', q([r[0] for r in rows]))
print('VU job busy per frame:         ', q([r[1] for r in rows]))
print('GIF stage busy per frame:      ', q([r[2] for r in rows]))
print('EE wait in vu1Busy per frame:  ', q([r[3] for r in rows]))
print('VU jobs per frame: median', st.median([r[4] for r in rows]))
print('job length:', q([e - s for s, e in vu]))
# gap between VU end and next VU start (VU idle) and what the EE does then
gaps = [vu[i+1][0] - vu[i][1] for i in range(len(vu)-1)]
print('VU idle gaps:', q(gaps), 'sum per s', sum(gaps)/((t1-t0)/1e6)/1e3, 'ms')
# kick latency: K (EE) -> S (VU)
ks = [e[0] for e in ev if e[1]=='K']; ss = [s for s,_ in vu]
lat = [s - k for k, s in zip(ks, ss) if s >= k]
print('kick->start latency:', q(lat))
# GIF tail after the frame's last VU job ends
