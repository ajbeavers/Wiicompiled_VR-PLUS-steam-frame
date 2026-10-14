# Shared helpers for the panel recording analysis (no numpy/PIL: audioop does the per-pixel work in C).
# Input: a directory made by extract.sh (L.gray, R.gray, pts.txt, geometry.txt).
import audioop, mmap, os, warnings

warnings.filterwarnings('ignore', category=DeprecationWarning)


class Eyes:
    def __init__(self, d):
        self.w, self.h = map(int, open(os.path.join(d, 'geometry.txt')).read().split())
        self.fs = self.w * self.h
        self.pts = [float(l) for l in open(os.path.join(d, 'pts.txt')) if l.strip()]
        self.maps = {}
        for e in 'LR':
            f = open(os.path.join(d, e + '.gray'), 'rb')
            self.maps[e] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        self.n = min(len(self.pts), len(self.maps['L']) // self.fs, len(self.maps['R']) // self.fs)
        # Refresh index of each frame (the recorder stamps frames with their vblank; 120 Hz panels).
        self.idx = [round(t * 120.0) for t in self.pts[:self.n]]

    def frame(self, e, i):
        o = i * self.fs
        return self.maps[e][o:o + self.fs]

    def region(self, e, i, x0, y0, w, h):
        f = self.frame(e, i)
        W = self.w
        return b''.join(f[(y0 + y) * W + x0:(y0 + y) * W + x0 + w] for y in range(h))


def to16(b):
    """unsigned 8-bit gray -> signed 16-bit samples halved (so differences never overflow)."""
    return audioop.mul(audioop.lin2lin(audioop.bias(b, 1, -128), 1, 2), 2, 0.5)


def rmsdiff16(a, b):
    """RMS difference in gray levels of two to16() fragments."""
    return audioop.rms(audioop.add(a, audioop.mul(b, 2, -1), 2), 2) / 128.0


def shifted(img, W, x0, y0, w, h, dx, dy):
    return b''.join(img[(y0 + dy + y) * W + x0 + dx:(y0 + dy + y) * W + x0 + dx + w] for y in range(h))
