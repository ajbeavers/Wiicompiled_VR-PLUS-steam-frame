#!/usr/bin/env python3
"""Mock check of the session-13 replay: python3 -I replaycheck.py <run-probes PROBE_MOCK=1 output dir> [case ...]

Per case (xrprobe log <case>.log, mock trace <case>.trace, dumps <case>.dump/ with MOCK_XR_DUMP_AT):
- frame order: the calls of each frame in the mock trace (waitFrame, beginFrame, locate +refreshes, acquire,
  release, endFrame with its layers), the commonest patterns and their share;
- display times: endFrame display time against the frame's predicted one; layer poses (from no locate:
  the recorded ones) and fields of view as submitted;
- images: each dumped (released) eye image against the recorded image of the set the probe showed in that
  frame (R lines), resampled as the probe's linear blit does (in linear light), mean absolute difference in
  8-bit steps outside the label strip; the neighbouring sets k-1 and k+1 for scale.
"""
import os, re, sys
from collections import Counter

D = sys.argv[1]
CASES = sys.argv[2:] or sorted(f[:-4] for f in os.listdir(D) if f.endswith('.log'))
TO_LIN = [((c / 255) / 12.92 if c / 255 <= 0.04045 else ((c / 255 + 0.055) / 1.055) ** 2.4) for c in range(256)]


def to_srgb(v):
    v = min(max(v, 0.0), 1.0)
    return 255 * (v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055)


def ppm(path):
    b = open(path, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', b)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, b[m.end():m.end() + w * h * 3]


for case in CASES:
    log = open(os.path.join(D, case + '.log')).read()
    m = re.search(r'replay: window (\S+), (\d+) sets of (\d+)x(\d+)', log)
    win, nsets, rw, rh = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))
    shown = {int(a): int(b) for a, b in re.findall(r'R n=(\d+) \S+ \S+ set=(-?\d+)', log)}
    # frame order
    frames, cur = {}, None
    for line in open(os.path.join(D, case + '.trace')):
        mm = re.match(r'f(\d+) (\S+)(.*?)( @[-\d.]+)?$', line.rstrip('\n'))
        if not mm or line.startswith('f0 ') or mm.group(2) in ('dump',):
            continue
        f, call, rest = int(mm.group(1)), mm.group(2), mm.group(3)
        if call == 'locate':
            call += re.search(r'display=(\S+)', rest).group(1)
        elif call == 'endFrame':
            call += '[' + ' '.join(re.findall(r'(proj|quad)\[', rest)) + ']'
        elif call in ('acquire', 'release', 'waitImage'):
            call += ' ' + rest.split()[0]
        elif call in ('endInfo', 'layer0', 'layer1', 'view0', 'view1') or call.startswith(('view', 'layer')):
            continue
        frames.setdefault(f, []).append(call)
    pats = Counter(' > '.join(re.sub(r' sc\d+', '', c) for c in v) for f, v in frames.items() if f > 5)
    total = sum(pats.values())
    tr = open(os.path.join(D, case + '.trace')).read()
    ends = len(re.findall(r'endInfo .*display=\+0\.0000ms', tr)), len(re.findall(r'endInfo ', tr))
    nopose = len(re.findall(r'proj space=s0:LOCAL .* poses from no known locate', tr)), len(re.findall(r'layer0 proj ', tr))
    fovs = Counter(re.findall(r'(view\d) sc\d+ .* fov=(\S+) @', tr))
    print(f'== {case}: window {win}, {nsets} sets of {rw}x{rh}, {len(frames)} frames in the trace')
    for p, c in pats.most_common(3):
        print(f'  {100 * c / total:5.1f} %  {p}')
    print(f'  end display time = predicted: {ends[0]} of {ends[1]}; projection layers with poses from no locate '
          f'(the recorded ones): {nopose[0]} of {nopose[1]}; fov ' + ', '.join(f'{v} {f} x{c}' for (v, f), c in sorted(fovs.items())))
    # images
    dd = os.path.join(D, case + '.dump')
    if not os.path.isdir(dd):
        continue
    idx_src = re.search(r'recording (\S+),', open(os.path.join(D, case + '.stdout')).read()).group(1)
    sets = {}
    for line in open(os.path.join(idx_src, 'index.txt')):
        p = line.split()
        if p[0].startswith(win + '-') and p[3] in '01' and int(p[4]) > 0:
            sets.setdefault(int(p[4]), {})[int(p[3])] = p[0]
    order = [sets[t] for t in sorted(sets) if len(sets[t]) == 2][:nsets]
    res = []
    for line in open(os.path.join(dd, 'index.txt')):
        f, sc, fr = line.split()[:3]
        sc, fr = int(sc[2:]), int(fr[1:])
        k = shown.get(fr, -1)
        if sc >= 2 * 8 or k < 0:
            continue
        w, h, px = ppm(os.path.join(dd, f))
        if w * 8 != 1728 or rw != 648:
            continue
        eye = sc % 2
        scores = []
        for kk in (k, (k - 1) % nsets, (k + 1) % nsets):
            _, _, src = ppm(os.path.join(idx_src, order[kk][eye]))
            tot, cnt = 0.0, 0
            for j in range(0, h, 2):
                if 0.15 * h <= j <= 0.30 * h:
                    continue
                y0, y1 = max(3 * j - 1, 0), 3 * j
                for i in range(0, w, 2):
                    x0, x1 = max(3 * i - 1, 0), 3 * i
                    for c in range(3):
                        a = lambda y, x: TO_LIN[src[(y * rw + x) * 3 + c]]
                        v = (0.3125 * (0.3125 * a(y0, x0) + 0.6875 * a(y0, x1)) + 0.6875 * (0.3125 * a(y1, x0) + 0.6875 * a(y1, x1)))
                        tot += abs(to_srgb(v) - px[(j * w + i) * 3 + c])
                        cnt += 1
            scores.append(tot / cnt)
        res.append((fr, eye, k, scores))
    if res:
        worst = max(r[3][0] for r in res)
        best = sum(1 for r in res if r[3][0] < min(r[3][1:]))
        print(f'  dumped eye images: {len(res)} (frames {min(r[0] for r in res)}-{max(r[0] for r in res)}); against the shown set: '
              f'mean abs diff max {worst:.2f}, closer than k-1 and k+1 in {best} of {len(res)}; '
              f'k+-1 mean {sum(sum(r[3][1:]) / 2 for r in res) / len(res):.2f}')
