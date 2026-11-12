"""K (Nov 12): Spanking on every woman with the seat tipped up (the pelvis, bone 17 about x: + = the seat up, as
stinger_women's), TILT degrees: eased in over the grab's first 8 frames, held through the spanks, out from frame 12 of
the finish to the launch at 21 (pairsim.wstage). Spanking is the women's own punish, so each women's body also gets an
"own" fit at 1.00 (the game's pairing, only the tilt) that plays with their prompt unchanged (punish_fit.cpp own).
Post-pass on resources/punish_fit.txt (re-run after emit7 and spank_women; idempotent): sets the victim keys' 4th value
on every women's Spanking block and adds the own 1.00 blocks.

    python -I spank_tilt.py"""
import sys, os, json
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
TILT = 30.0
FILE = os.path.join(HERE, '..', '..', '..', 'resources', 'punish_fit.txt')
STAGE = {1327: 0, 1329: 1, 1331: 2}
KEYS = {1327: [0, 4, 8, 12, 16, 20, 22], 1329: [0, 4, 8, 12, 16], 1331: [0, 4, 8, 12, 16, 20]}
def w(si, k):
    if si == 0: return min(1.0, k / 8.0)
    if si == 1: return 1.0
    return 1.0 if k <= 12 else max(0.0, (21 - k) / 9.0)
def item(m, f, v):
    v = (v + [0.0] * 5)[:max(5, len(v))]; v[3] = TILT * w(STAGE[m], f)
    return '%d:' % f + ','.join(('%.3f' if i in (0, 2, 5) else '%.1f' if i == 3 else '%.2f') % x for i, x in enumerate(v))
def main():
    B = json.load(open(os.path.join(HERE, 'bodies.json')))
    women = {','.join(b['keys']): b['files'][0] for b in B if b.get('woman')}
    L = open(FILE).read().split('\n'); out = []; cur = None; have = set()
    for l in L:
        p = l.split()
        if p[:1] == ['fit']:
            cur = p[2] if p[1] == 'Spanking' and p[2] in women and 'off' not in p else None
            if cur and 'own' in p and 'scale=1.00' in p: have.add(cur)
        elif cur and p[:1] == ['victim'] and int(p[1]) in STAGE:
            m = int(p[1]); l = 'victim %d ' % m + ' '.join(item(m, int(x.split(':')[0]), [float(y) for y in x.split(':')[1].split(',')]) for x in p[2:])
        out.append(l)
    for keys, file in women.items():
        if keys in have: continue
        out.append('fit Spanking %s own scale=1.00 size=1.000   # %s (women): the game\'s own pairing, the seat tipped up %.0f deg (K; spank_tilt.py)' % (keys, file, TILT))
        for m, fs in KEYS.items(): out.append('victim %d ' % m + ' '.join(item(m, f, [0.0, 0.0]) for f in fs))
    while out and out[-1] == '': out.pop()
    open(FILE, 'w').write('\n'.join(out) + '\n')
    print('tilt %.0f on %d women bodies; own 1.00 blocks added: %d' % (TILT, len(women), len(women) - len(have)))
if __name__ == '__main__': main()
