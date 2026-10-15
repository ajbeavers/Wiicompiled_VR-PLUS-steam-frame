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
scene = collections.defaultdict(dict)  # window -> {display ns: (weight, previous, current game frame)} (left eye)
for line in open(os.path.join(D, 'index.txt')):
    parts = line.split()
    f, sc, fr, eye, val = parts[:5]
    if int(eye) < 0 or not os.path.exists(os.path.join(D, f)):
        continue
    wins[f.split('-')[0]].setdefault('LR'[int(eye)], {})[int(val)] = f
    if len(parts) >= 7 and int(eye) == 0 and (int(parts[5]) >> 48) & 1:
        a = int(parts[5])
        scene[f.split('-')[0]][int(val)] = ((a & 0xffff) / 65535.0, (a >> 16) & 0xffff, (a >> 32) & 0xffff)
PERIOD = 1e9 / float(os.environ.get('MOCK_XR_HZ', '120'))


def rule(pos):
    """H7's zigzag rule on a position track {refresh: px}: (tests, zigzags, mean |dev| in refreshes of motion,
    lag-1 autocorrelation of the deviation, speed median, |dev| p95 over all sets, deviations by refresh)."""
    tests = zz = 0
    amps, z, vs, allz = [], {}, [], []
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
            continue
        tests += 1
        z[n] = zp
        if abs(zp) >= 1 and abs(zp / v) >= 0.75:
            zz += 1
            amps.append(abs(zp / v))
    a1 = [(z[k], z[k + 1]) for k in sorted(z) if k + 1 in z]
    q = sorted(allz)
    return dict(tests=tests, zz=zz, amp=statistics.fmean(amps) if amps else 0.0, ac1=corr([x for x, _ in a1], [y for _, y in a1]),
                speed=statistics.median(vs) if vs else 0.0, p95=q[int(0.95 * (len(q) - 1))] if q else float('nan'),
                ge1=sum(1 for x in q if x >= 1), n=len(q), z=z)


def track(items, profs, axis):
    pos, p, prevn = {}, 0.0, None
    for n, _ in items:
        if prevn is not None:
            p += fit(profs[prevn][axis], profs[n][axis])
        pos[n] = p
        prevn = n
    return pos


for win in sorted(wins):
    eyes = wins[win]
    if 'L' not in eyes or 'R' not in eyes:
        continue
    v0 = min(min(eyes['L']), min(eyes['R']))
    tracks, lag = {}, {}
    for e in 'LR':
        items = sorted((round((v - v0) / PERIOD), f) for v, f in eyes[e].items())
        profs, cen = {}, {}
        for n, f in items:
            W, H, g = ppm_gray(os.path.join(D, f))
            # view: columns of the sky, hills and track edge (head and kart turning; clear of the stamp);
            # left / right: columns of the track-side strips at the horizon (fences, banners): forward motion
            # moves them apart (scene: (right - left) / 2), turning moves them together.
            x0, y0, rw, rh = W // 10, H // 12, W * 8 // 10, H * 3 // 10
            sy0, sh, sw = H * 28 // 100, H * 18 // 100, W * 30 // 100
            profs[n] = (cols(region(g, W, x0, y0, rw, rh), rw, rh), cols(region(g, W, 2, sy0, sw, sh), sw, sh),
                        cols(region(g, W, W - 2 - sw, sy0, sw, sh), sw, sh))
            cen[n] = to16(region(g, W, W // 4, H // 8, W // 2, H // 4))
        tl, tr = track(items, profs, 1), track(items, profs, 2)
        tracks[e] = {'view': track(items, profs, 0), 'scene': {n: 0.5 * (tr[n] - tl[n]) for n in tl}}
        lag[e] = {n: (rmsdiff16(cen[n], cen[n - 1]) if n - 1 in cen else None, rmsdiff16(cen[n], cen[n - 2]) if n - 2 in cen else None) for n in cen}
    span = max(max(tracks['L']['view']), max(tracks['R']['view'])) + 1
    print(f"== window {win}: sets L {len(tracks['L']['view'])}, R {len(tracks['R']['view'])} over {span} refreshes")
    # Scene position per set from the left eye's stamp (game frames).
    sp = {}
    for v, (w, prev, cur) in scene.get(win, {}).items():
        span_f = (cur - prev) & 0xffff
        sp[round((v - v0) / PERIOD)] = (cur - span_f + w * span_f, w, cur)
    pos_s = {}
    if sp:
        ks = sorted(sp)
        base = sp[ks[0]][2]
        pos_s = {k: sp[k][0] + 65536 * round((base - sp[k][2]) / 65536) for k in ks}
        steps = [pos_s[k] - pos_s[k - 1] for k in ks if k - 1 in pos_s]
        alt = sum(1 for k in ks if k - 1 in pos_s and k - 2 in pos_s and abs((pos_s[k] - pos_s[k - 1]) - (pos_s[k - 1] - pos_s[k - 2])) >= 0.3)
        ws = collections.Counter(round(sp[k][1], 2) for k in ks)
        print(f"  scene stamp: steps between sets one refresh apart mean {statistics.fmean(steps):.3f} sd {statistics.pstdev(steps):.3f} game frames; "
              f"consecutive steps differing by 0.3+ {alt} of {len(steps)}; weight 1.0 in {ws.get(1.0, 0)}, 0.0 in {ws.get(0.0, 0)} sets")
    devs = {}
    for kind in ('view', 'scene'):
        for e in 'LR':
            r = rule(tracks[e][kind])
            devs[kind, e] = r['z']
            pos = tracks[e][kind]
            st = [(pos[k] - pos[k - 1], pos_s[k] - pos_s[k - 1]) for k in pos if k - 1 in pos and k in pos_s and k - 1 in pos_s]
            cs = corr([a for a, _ in st], [b for _, b in st]) if st else float('nan')
            print(f"  {kind:6s} {e}: speed p50 {r['speed']:.2f} px/refresh; |dev| p95 {r['p95']:.2f} px (>=1 px {r['ge1']} of {r['n']}); moving {r['tests']}, "
                  f"zigzag {r['zz']} ({100.0 * r['zz'] / max(r['tests'], 1):.1f} %, mean {r['amp']:.2f} refreshes); lag-1 autocorr {r['ac1']:+.2f}; "
                  f"step vs scene step corr {cs:+.2f}")
        common = [n for n in devs[kind, 'L'] if n in devs[kind, 'R']]
        print(f"  {kind:6s} eyes: deviation L vs R corr {corr([devs[kind, 'L'][n] for n in common], [devs[kind, 'R'][n] for n in common]):+.2f} over {len(common)} moving sets")
    for e in 'LR':
        l1 = [x for x, y in lag[e].values() if x is not None]
        l2 = [y for x, y in lag[e].values() if y is not None]
        stale = sum(1 for x, y in lag[e].values() if x is not None and y is not None and y < 0.5 * x)
        ident = sum(1 for x, y in lag[e].values() if (x is not None and x < 0.5) or (y is not None and y < 0.5))
        print(f"  lagdiff {e}: vs set before p50 {statistics.median(l1) if l1 else 0:.1f}, two before p50 {statistics.median(l2) if l2 else 0:.1f} "
              f"gray levels; nearer n-2 than half of n-1: {stale}; identical (<0.5) to n-1 or n-2: {ident}")
    if os.environ.get('ZZ_TRACK'):
        for n in range(span):
            print('   ', n, ' '.join(f"{tracks[e][k].get(n, float('nan')):8.2f}" for k in ('view', 'scene') for e in 'LR'),
                  f"{pos_s.get(n, float('nan')):.3f}", sp.get(n, ('',))[1:2])
