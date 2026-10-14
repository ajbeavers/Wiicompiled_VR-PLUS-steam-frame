# cadence.py LAGDIFF [MOTION] [RACE_FROM_S]: per eye, how each recorded refresh relates to the ones before it
# (from lagdiff.py's table; MOTION from motion.py adds the split by image motion):
#   new    differs from every earlier refresh it was compared with,
#   rep    the previous refresh again (a held frame; only seen while nothing moves on the panel),
#   stale  an image the eye showed 2-8 refreshes before, pixel for pixel, while the previous refresh
#          differed: an old image shown again where it was shown then, i.e. with a newer frame's pose.
# Also both eyes together (out of step: one stale or held while the other is new) and the lag of the stale ones.
import collections, sys

lag = sys.argv[1]
mot_path = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != '-' else None
race_from = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
D, T = {}, {}
for l in open(lag):
    r = l.split()
    D[(int(r[1]), r[2])] = [float(x) for x in r[3:]]
    T[int(r[1])] = float(r[0])


def cls(i, e):
    d = D.get((i, e))
    if d is None or d[0] < 0:
        return None, 0
    if d[0] < 2.0:
        return 'rep', 1
    for k in range(1, 8):
        if 0 <= d[k] <= 4.0 and d[k] < 0.35 * d[0]:
            return 'stale', k + 1
    return 'new', 0


mot = {}
if mot_path:
    for l in open(mot_path):
        t, i, e, g, dx, dy = l.split()
        if g == '1':
            mot[(int(i), e)] = abs(float(dx))
idx = sorted(T)
for name, keep in (('before ' + str(race_from) + ' s', lambda t: t < race_from), ('from ' + str(race_from) + ' s', lambda t: t >= race_from)):
    sel = [i for i in idx if keep(T[i])]
    print('==', name)
    for e in 'LR':
        c = collections.Counter(); lags = collections.Counter(); holds = collections.Counter(); run = 1
        speed = collections.defaultdict(collections.Counter)
        for i in sel:
            k, lg = cls(i, e)
            if k is None:
                run = 1
                continue
            c[k] += 1
            if k == 'stale':
                lags[lg] += 1
            if k == 'rep':
                run += 1
            else:
                holds[run] += 1
                run = 1
            if mot:
                v = [mot[(j, e)] for j in range(i - 4, i + 1) if (j, e) in mot]
                if v:
                    s = sum(v) / len(v)
                    b = 'image still (<0.5 px/refresh)' if s < 0.5 else 'slow (0.5-2)' if s < 2 else 'moving (>=2)'
                    speed[b][k] += 1
        n = sum(c.values()) or 1
        print('%s: %d refreshes; new %.1f %%, held %.1f %%, stale %.1f %% (lags %s); holds %s' % (
            e, n, 100 * c['new'] / n, 100 * c['rep'] / n, 100 * c['stale'] / n,
            ' '.join('%d:%d' % kv for kv in sorted(lags.items())), ' '.join('%d:%d' % kv for kv in sorted(holds.items())[:6])))
        for b, cc in sorted(speed.items()):
            m = sum(cc.values())
            print('   %-30s %6d refreshes, stale %.1f %%' % (b, m, 100 * cc['stale'] / m))
    both = collections.Counter()
    for i in sel:
        a, _ = cls(i, 'L'); b, _ = cls(i, 'R')
        if a and b:
            both[(a, b)] += 1
    n = sum(both.values()) or 1
    out = sum(v for (a, b), v in both.items() if a != b)
    print('both eyes: %d refreshes, out of step %.1f %%, stale together %.2f %%; %s' % (
        n, 100 * out / n, 100 * both[('stale', 'stale')] / n, ' '.join('%s/%s:%d' % (a, b, v) for (a, b), v in sorted(both.items()))))
