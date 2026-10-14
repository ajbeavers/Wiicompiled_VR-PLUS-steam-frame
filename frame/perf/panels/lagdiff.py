# lagdiff.py DIR [T0 T1] > table: per eye and captured frame, the RMS difference (gray levels) of a central
# region against the frames 1..8 refreshes earlier (when the recorder caught them), without motion
# compensation. Columns: t idx eye d1..d8 (-1 = not captured).
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pgray import Eyes, to16, rmsdiff16

d = sys.argv[1]
t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
t1 = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
E = Eyes(d)
X0, Y0, RW, RH = 100, 40, 280, 110   # above the head-locked overlay, inside both lens circles
for e in 'LR':
    cache = {}
    for i in range(E.n):
        t = E.pts[i]
        if t < t0 - 0.1 or t > t1:
            continue
        cache[E.idx[i]] = to16(E.region(e, i, X0, Y0, RW, RH))
        for k in [k for k in cache if k < E.idx[i] - 8]:
            del cache[k]
        if t < t0:
            continue
        cur = cache[E.idx[i]]
        row = []
        for lag in range(1, 9):
            p = cache.get(E.idx[i] - lag)
            row.append('%.2f' % rmsdiff16(cur, p) if p is not None else '-1')
        print('%.3f %d %s %s' % (t, E.idx[i], e, ' '.join(row)))
