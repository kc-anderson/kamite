"""Triangle list -> the game's strip vertex buffers (flags: 0x8000 = no triangle, 0/1 = triangle winding).

Each triangle carries its own winding flag, so a strip may cross the edge between its last two vertices whatever the
winding. Strips are grown both ways from a start triangle (the unused triangle with the fewest free neighbours, so
stranded triangles are taken first), from whichever of its three edges gives the longest strip. A strip costs its
triangle count + 2 vertices. swaps=True also lets a strip turn onto the other free edge of its last triangle by
re-sending the third-last vertex as a no-triangle vertex (1 vertex instead of a 2-vertex restart); the game's own
models never send a single no-triangle vertex, so it is off unless asked for."""
from collections import defaultdict


def stripify(tris, swaps=False, tries=40):
    """tris: list of (a,b,c) unique-vertex ids with the wanted winding. Returns strips, each a list of
    (vertex id, makes_triangle); the first two entries of a strip never make a triangle. The greedy pass is run
    'tries' times with ties broken at random (seeded, so the output is the same every run); the fewest vertices win."""
    import random
    best = None
    for k in range(max(1, tries)):
        strips = _stripify(tris, swaps, random.Random(k) if k else None)
        cost = sum(len(s) for s in strips)
        if best is None or cost < best[0]:
            best = (cost, strips)
    return best[1]


def _stripify(tris, swaps, rng):
    edge_tris = defaultdict(list)
    for t, (a, b, c) in enumerate(tris):
        for e in ((a, b), (b, c), (c, a)):
            edge_tris[frozenset(e)].append(t)
    used = [False] * len(tris)

    def across(t, p, q):
        for u in edge_tris[frozenset((p, q))]:
            if u != t and not used[u]:
                x = [v for v in tris[u] if v != p and v != q]
                if len(x) == 1:
                    return u, x[0]
        return None

    def degree(t):
        a, b, c = tris[t]
        return sum(1 for e in ((a, b), (b, c), (c, a)) for u in edge_tris[frozenset(e)] if u != t and not used[u])

    def grow(seq, t, allow_swaps):
        """Extend seq (its last three entries are triangle t) forward, marking what it takes; returns those."""
        taken = []
        while True:
            nxt = across(t, seq[-2][0], seq[-1][0])
            if nxt is None and allow_swaps:
                nxt = across(t, seq[-3][0], seq[-1][0])
                if nxt is not None:
                    seq.append((seq[-3][0], False))
            if nxt is None:
                return taken
            t, x = nxt
            used[t] = True
            taken.append(t)
            seq.append((x, True))

    def build(start, rot, keep):
        a, b, c = tris[start][rot:] + tris[start][:rot]
        used[start] = True
        back = [(c, False), (b, False), (a, True)]
        took_b = grow(back, start, False)
        seq = _reverse(back)                       # ... a, b, c: ends with the start triangle
        took_f = grow(seq, start, swaps)
        if not keep:
            for u in took_b + took_f + [start]:
                used[u] = False
        return seq, 1 + len(took_b) + len(took_f)

    strips = []
    left = set(range(len(tris)))
    while left:
        if rng is None:
            start = min(left, key=degree)
        else:
            d = {t: degree(t) for t in left}; m = min(d.values())
            start = rng.choice(sorted(t for t in left if d[t] == m))
        rots = [0, 1, 2]
        if rng is not None:
            rng.shuffle(rots)
        best = max(rots, key=lambda r: (lambda s, n: (n, -len(s)))(*build(start, r, False)))
        seq, _ = build(start, best, True)
        strips.append(seq)
        left = {t for t in left if not used[t]}
    return strips


def _reverse(back):
    """A strip without swaps grown backwards, turned round: every entry from the third on makes a triangle."""
    verts = [v for v, _ in back][::-1]
    return [(v, i >= 2) for i, v in enumerate(verts)]


def build_buffers(tris, max_verts=1000, swaps=False):
    """Returns list of buffers: each [(vid, flag)] with flags 0x8000/0/1, keeping each buffer <= max_verts."""
    want = set()
    for a, b, c in tris:
        want.add((a, b, c)); want.add((b, c, a)); want.add((c, a, b))
    bufs = []; cur = []
    for s in stripify(tris, swaps):
        if len(cur) + 3 > max_verts:
            bufs.append(cur); cur = []
        last = []   # the last two vertices sent; a buffer split restarts the strip from them
        for v, makes in s:
            if makes:
                if len(cur) + 1 > max_verts:
                    bufs.append(cur); cur = [(last[0], 0x8000), (last[1], 0x8000)]
                cur.append((v, 0 if (last[0], last[1], v) in want else 1))
            else:
                cur.append((v, 0x8000))
            last = (last + [v])[-2:]
    if cur:
        bufs.append(cur)
    return bufs
