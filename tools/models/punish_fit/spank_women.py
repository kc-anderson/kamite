"""K (Nov 12): Spanking on the bigger women (group 1, around size 1.15): the game's own pairing put Gene's holding (left)
arm ~8 cm into the larger torso. An exception to "don't pose the game's own punish" (K: one small tweak): his left
arm raised in the clavicle's space and the elbow a little opened, held through the grab (eased in over its first 8
frames), the spanks and the finish (out from frame 12 to the launch), still hugging her hips like the original. Chosen
by a grid over the two angles at size 1.15 (pairsim): the arm no deeper in her than the original pairing at 1.00 +
0.5 cm, the hand's touch on her hips like the original's. K picks between variants in play.

    GODHAND_EXTRACT=<extract> python -I spank_women.py [--patch]   (fits7/g_Spanking_01_1.15; --patch: into the file)"""
import sys, os, json, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import pairsim as ps
from spank130 import KS
SCALE = 1.15; RAISE, ELBOW = 28.0, -10.0   # K's variant B (Nov 12)
NOTE = "Gene's left arm raised %.0f deg, the elbow %.0f deg (K: the bigger women; spank_women.py)" % (RAISE, ELBOW)
def save():
    from fit_pose import OUT
    X = np.zeros((sum(map(len, KS)), 14)); o = 0
    for si, fs in enumerate(KS):
        for f in fs: X[o, 8] = np.radians(RAISE) * ps.wstage(si, f); X[o, 10] = np.radians(ELBOW) * ps.wstage(si, f); o += 1
    m = dict(name='Spanking', type='women', group=1, scale=SCALE, rep=False, own_placement=True, ks=KS, size=1.0, note=NOTE)
    np.save(os.path.join(OUT, 'g_Spanking_01_%.2f.npy' % SCALE), X); json.dump(m, open(os.path.join(OUT, 'g_Spanking_01_%.2f.json' % SCALE), 'w'), indent=1)
    return m, X
def patch(path=os.path.join(HERE, '..', '..', '..', 'resources', 'punish_fit.txt')):
    import emit7
    from fit_pose import B
    m, X = save(); keys = ','.join(B[1]['keys']); block = emit7.own_block('Spanking', keys, B[1]['files'][0], 'women', SCALE, m, X)
    L = open(path).read().split('\n'); out = []; i = 0; done = False
    while i < len(L):
        p = L[i].split()
        if p[:3] == ['fit', 'Spanking', keys] and 'scale=%.2f' % SCALE in p:   # an earlier copy: replaced
            i += 1
            while i < len(L) and L[i].split()[:1] in (['gene'], ['victim']): i += 1
            continue
        if p[:3] == ['fit', 'Spanking', keys] and 'scale=1.30' in p and not done: out += block; done = True
        out.append(L[i]); i += 1
    open(path, 'w').write('\n'.join(out)); print('inserted' if done else 'NOT FOUND')
if __name__ == '__main__':
    patch() if '--patch' in sys.argv else save()
