# zigzag.py DUMPDIR [X0 Y0 W H]: the panel zigzag test (H7 section 1, session-h12 zig2.py) on mock eye images.
# DUMPDIR: MOCK_XR_DUMP output with index.txt (xrmock with MOCK_XR_STAMP: eye and display time per image).
# Per dump window (t<seconds> prefix) and eye: the global image shift between consecutive sets from column
# projections of a region of the upper image (motion.py's fit), the position track, H7's zigzag rule on
# sets n-1, n, n+1 one refresh apart (moving: |speed| >= 2 px a refresh over n-4..n+5; zigzag: |pos(n) -
# mean(pos(n-1), pos(n+1))| >= 1 px and >= 0.75 refresh of motion), the lag-1 autocorrelation of that
# deviation (period 2: near -1), the two eyes' deviations against each other (in phase: +1, against: -1),
# and a lagdiff: per set the RMS difference of a central region against the same eye's set 1 and 2 before,
# plus how often the image sits where set n-2 was (|pos(n) - pos(n-2)| < 0.25 px while moving).
# Pixels: the dump's (MOCK_XR_DUMP_STEP 4: 432 px across 1728).
import os, sys, audioop, collections, statistics, warnings
warnings.filterwarnings('ignore')
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'panels'))
from pgray import to16, rmsdiff16

D = sys.argv[1]
M = 24


def ppm_gray(path):
    data = open(path, 'rb').read()
    head = data.split(b'\n', 3)
    w, h = map(int, head[1].split())
    return w, h, data[len(data) - w * h * 3:][1::3]  # green channel


def region(img, W, x0, y0, w, h):
    return b''.join(img[(y0 + y) * W + x0:(y0 + y) * W + x0 + w] for y in range(h))


def cols(reg, w, h):
    c = [0] * w
    for y in range(h):
        r = reg[y * w:(y + 1) * w]
        for x in range(w):
            c[x] += r[x]
    return c


def to16v(vals):
    m = sum(vals) / len(vals)
    s = max(1.0, max(abs(v - m) for v in vals))
    return b''.join(int((v - m) * 12000 / s).to_bytes(2, 'little', signed=True) for v in vals)


def fit(prev, cur):
    n = len(cur)
    ref = to16v(cur[M:n - M])
    frag = to16v(prev)
    off, _ = audioop.findfit(frag, ref)
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


def corr(a, b):
    if len(a) < 3:
        return float('nan')
    ma, mb = statistics.fmean(a), statistics.fmean(b)
    sa = sum((x - ma) ** 2 for x in a) ** 0.5
    sb = sum((y - mb) ** 2 for y in b) ** 0.5
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (sa * sb) if sa > 0 and sb > 0 else float('nan')


# index.txt: file sc frame eye displayns
wins = collections.defaultdict(dict)  # window -> eye -> {display ns: file}
for line in open(os.path.join(D, 'index.txt')):
    f, sc, fr, eye, val = line.split()
    if int(eye) < 0 or not os.path.exists(os.path.join(D, f)):
        continue
    wins[f.split('-')[0]].setdefault('LR'[int(eye)], {})[int(val)] = f
PERIOD = 1e9 / float(os.environ.get('MOCK_XR_HZ', '120'))
for win in sorted(wins):
    eyes = wins[win]
    if 'L' not in eyes or 'R' not in eyes:
        continue
    v0 = min(min(eyes['L']), min(eyes['R']))
    track, dev, lag = {}, {}, {}
    for e in 'LR':
        items = sorted((round((v - v0) / PERIOD), f) for v, f in eyes[e].items())
        W = H = None
        prof, cen = {}, {}
        for n, f in items:
            W, H, g = ppm_gray(os.path.join(D, f))
            if len(sys.argv) > 5:
                x0, y0, rw, rh = map(int, sys.argv[2:6])
            else:
                x0, y0, rw, rh = W // 10, W // 12, W * 8 // 10, W * 3 // 10  # sky, hills, track edge; not the stamp
            prof[n] = cols(region(g, W, x0, y0, rw, rh), rw, rh)
            cen[n] = to16(region(g, W, W // 4, W // 8, W // 2, W // 4))
        pos, p, prevn = {}, 0.0, None
        for n, _ in items:
            if prevn is not None:
                p += fit(prof[prevn], prof[n])
            pos[n] = p
            prevn = n
        track[e] = pos
        lag[e] = {n: (rmsdiff16(cen[n], cen[n - 1]) if n - 1 in cen else None, rmsdiff16(cen[n], cen[n - 2]) if n - 2 in cen else None) for n in cen}
    span = max(max(track['L']), max(track['R'])) + 1
    print(f"== window {win}: sets L {len(track['L'])}, R {len(track['R'])} over {span} refreshes (gaps: L {span - len(track['L'])}, R {span - len(track['R'])})")
    for e in 'LR':
        pos = track[e]
        tests = zz = 0
        amps, z, vs, still, copy2, allz = [], {}, [], [], 0, []
        for n in sorted(pos):
            if n - 1 not in pos or n + 1 not in pos:
                continue
            zp = pos[n] - 0.5 * (pos[n - 1] + pos[n + 1])
            allz.append(abs(zp))
            ks = [k for k in range(n - 4, n + 6) if k in pos]
            if ks[-1] - ks[0] < 4:
                continue
            v = (pos[ks[-1]] - pos[ks[0]]) / (ks[-1] - ks[0])
            vs.append(abs(v))
            if abs(v) < 2:
                still.append(abs(zp))
                continue
            tests += 1
            z[n] = zp
            if n - 2 in pos and abs(pos[n] - pos[n - 2]) < 0.25:
                copy2 += 1
            if abs(zp) >= 1 and abs(zp / v) >= 0.75:
                zz += 1
                amps.append(abs(zp / v))
        dev[e] = z
        ks = sorted(z)
        a1 = [(z[k], z[k + 1]) for k in ks if k + 1 in z]
        ac1 = corr([x for x, _ in a1], [y for _, y in a1])
        q = sorted(allz)
        p95 = q[int(0.95 * (len(q) - 1))] if q else float('nan')
        print(f"  {e}: speed median {statistics.median(vs) if vs else 0:.2f} px/refresh; |dev| all sets p50 {statistics.median(q) if q else 0:.2f} "
              f"p95 {p95:.2f} px, >=1 px {sum(1 for x in q if x >= 1)} of {len(q)}; moving tests {tests}, zigzag {zz} "
              f"({100.0 * zz / max(tests, 1):.1f} %), mean |dev| {statistics.fmean(amps) if amps else 0:.2f} refreshes; lag-1 autocorr "
              f"{ac1:+.2f}; at set n-2's position {copy2}")
        l1 = [x for x, y in lag[e].values() if x is not None]
        l2 = [y for x, y in lag[e].values() if y is not None]
        stale = sum(1 for x, y in lag[e].values() if x is not None and y is not None and y < 0.5 * x)
        ident = sum(1 for x, y in lag[e].values() if (x is not None and x < 0.5) or (y is not None and y < 0.5))
        print(f"     lagdiff (gray levels): vs set before p50 {statistics.median(l1) if l1 else 0:.1f}, vs two before p50 "
              f"{statistics.median(l2) if l2 else 0:.1f}; closer to n-2 than half of n-1 {stale}; identical (<0.5) to n-1 or n-2 {ident}")
    common = [n for n in dev['L'] if n in dev['R']]
    print(f"  eyes: deviations L vs R corr {corr([dev['L'][n] for n in common], [dev['R'][n] for n in common]):+.2f} over {len(common)} sets; "
          f"position step L vs R corr {corr([track['L'][n] - track['L'][n - 1] for n in track['L'] if n - 1 in track['L'] and n in track['R'] and n - 1 in track['R']], [track['R'][n] - track['R'][n - 1] for n in track['L'] if n - 1 in track['L'] and n in track['R'] and n - 1 in track['R']]):+.2f}")
    if os.environ.get('ZZ_TRACK'):
        for n in range(span):
            print('   ', n, ' '.join(f"{track[e].get(n, float('nan')):8.2f}" for e in 'LR'))
