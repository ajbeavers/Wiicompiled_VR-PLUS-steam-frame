# motion.py DIR > table: per eye and captured frame, the global image shift (x, y in extract pixels: 480x270
# per eye, so 1 px = 4.5 panel px across, 8 down) against the previous captured frame, from column and row
# projections of the upper part of the lens circle (tunnel walls, sky: the world, not the HUD overlay).
# Columns: t idx eye gap dx dy.  gap = refreshes since the previous captured frame.
import os, sys, audioop
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pgray import Eyes

E = Eyes(sys.argv[1])
X0, Y0, RW, RH = 60, 20, 360, 130
M = 24  # search +-M px


def profiles(e, i):
    reg = E.region(e, i, X0, Y0, RW, RH)
    cols = [0] * RW
    rows = []
    for y in range(RH):
        r = reg[y * RW:(y + 1) * RW]
        rows.append(sum(r))
        for x in range(0, RW):
            cols[x] += r[x]
    return cols, rows


def to16(vals):
    m = sum(vals) / len(vals)
    s = max(1.0, max(abs(v - m) for v in vals))
    return b''.join(int((v - m) * 12000 / s).to_bytes(2, 'little', signed=True) for v in vals)


def fit(prev, cur):
    """Shift of cur against prev (positive: content moved right/down), with sub-pixel refinement."""
    n = len(cur)
    ref = to16(cur[M:n - M])
    frag = to16(prev)
    off, _ = audioop.findfit(frag, ref)  # ref best matches frag[off:off+n-2M]
    def err(o):
        if o < 0 or o > 2 * M:
            return None
        part = frag[2 * o:2 * (o + n - 2 * M)]
        f = audioop.findfactor(part, ref)
        return audioop.rms(audioop.add(part, audioop.mul(ref, 2, -f), 2), 2)
    e0, em, ep = err(off), err(off - 1), err(off + 1)
    sub = 0.0
    if em is not None and ep is not None and em + ep - 2 * e0 > 0:
        sub = 0.5 * (em - ep) / (em + ep - 2 * e0)
    return (M - off) - sub


import warnings
warnings.filterwarnings('ignore')
for e in 'LR':
    prev = None
    for i in range(E.n):
        cols, rows = profiles(e, i)
        if prev is not None:
            dx = fit(prev[0], cols)
            dy = fit(prev[1], rows) if len(rows) > 2 * M + 8 else 0.0
            print('%.3f %d %s %d %.2f %.2f' % (E.pts[i], E.idx[i], e, E.idx[i] - pidx, dx, dy))
        prev = (cols, rows)
        pidx = E.idx[i]
