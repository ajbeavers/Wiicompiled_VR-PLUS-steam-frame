#!/usr/bin/env python3
"""Compare PNG captures, with the standard library only (the Frame has no numpy or PIL).

  tools/imgdiff.py A.png B.png [--tolerant] [--diff out.png]
  tools/imgdiff.py DIR_A DIR_B [--tolerant] [--diff-dir DIR]

Two directories are compared file by file (every *.png name in either one; a name missing
on one side is a failure).  Exit status 0 when every pair passes, 1 otherwise.

Modes
  exact (default)  the decoded pixels must be identical (the PNG files themselves may
                   differ in compression).  Used for refactors that must not change a pixel.
  --tolerant       the pass rule for comparing renderer backends: mean absolute difference
                   under 1/255 (of full scale), and under 0.5% of the pixels differing by more
                   than 8/255 in any colour channel.  Alpha is ignored (the captures force it
                   to 255 in some paths and not in others).
  --ignore a,b     (directories) skip these names (without .png, and their _xr variants), and say so
  --mean T --frac F --pix N   override the tolerant limits (T in 1/255 units, F in percent,
                   N in 1/255 units).

Reads 8-bit grey, grey+alpha, RGB, RGBA and palette PNGs, not interlaced.
"""
import os
import struct
import sys
import zlib


def read_png(path):
    """Returns (width, height, rgba bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: " + path)
    pos = 8
    idat = []
    plte = None
    trns = None
    ihdr = None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat.append(body)
        elif typ == b"IEND":
            break
    if ihdr is None:
        raise ValueError("no IHDR: " + path)
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or interlace != 0 or ctype not in (0, 2, 3, 4, 6):
        raise ValueError("unsupported PNG (depth %d, type %d, interlace %d): %s" % (depth, ctype, interlace, path))
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = w * ch
    raw = zlib.decompress(b"".join(idat))
    if len(raw) != (stride + 1) * h:
        raise ValueError("bad PNG data length: " + path)
    out = bytearray(stride * h)
    prev = bytearray(stride)
    for y in range(h):
        base = y * (stride + 1)
        ft = raw[base]
        line = bytearray(raw[base + 1:base + 1 + stride])
        if ft == 0:
            pass
        elif ft == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif ft == 2:
            line = bytearray(((a + b) & 255) for a, b in zip(line, prev))
        elif ft == 3:
            for i in range(stride):
                left = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        else:
            raise ValueError("bad filter %d: %s" % (ft, path))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    # Normalise to RGBA.
    if ctype == 6:
        return w, h, bytes(out)
    rgba = bytearray(w * h * 4)
    if ctype == 2:
        rgba[0::4] = out[0::3]
        rgba[1::4] = out[1::3]
        rgba[2::4] = out[2::3]
        rgba[3::4] = b"\xff" * (w * h)
    elif ctype == 0:
        for k in range(3):
            rgba[k::4] = out
        rgba[3::4] = b"\xff" * (w * h)
    elif ctype == 4:
        for k in range(3):
            rgba[k::4] = out[0::2]
        rgba[3::4] = out[1::2]
    else:
        pal = [tuple(plte[i:i + 3]) for i in range(0, len(plte), 3)]
        for i, idx in enumerate(out):
            r, g, b = pal[idx]
            rgba[i * 4:i * 4 + 4] = bytes((r, g, b, trns[idx] if trns and idx < len(trns) else 255))
    return w, h, bytes(rgba)


def write_png(path, w, h, rgba):
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    stride = w * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def compare(pa, pb, limits, diff_out=None):
    """Returns (passed, text)."""
    wa, ha, a = read_png(pa)
    wb, hb, b = read_png(pb)
    if (wa, ha) != (wb, hb):
        return False, "size differs: %dx%d vs %dx%d" % (wa, ha, wb, hb)
    if a == b:
        return True, "identical"
    n = wa * ha
    # Per-pixel max channel difference and the colour channel sum, on RGB only.
    total = 0
    over = 0
    maxd = 0
    exact_diff_px = 0
    pix_t = limits["pix"]
    dimg = bytearray(n * 4) if diff_out else None
    for i in range(0, n * 4, 4):
        if a[i:i + 4] == b[i:i + 4]:
            if dimg is not None:
                dimg[i + 3] = 255
            continue
        d0 = abs(a[i] - b[i])
        d1 = abs(a[i + 1] - b[i + 1])
        d2 = abs(a[i + 2] - b[i + 2])
        total += d0 + d1 + d2
        m = max(d0, d1, d2)
        if m:
            exact_diff_px += 1
        if m > pix_t:
            over += 1
        if m > maxd:
            maxd = m
        if dimg is not None:
            dimg[i] = min(255, d0 * 8)
            dimg[i + 1] = min(255, d1 * 8)
            dimg[i + 2] = min(255, d2 * 8)
            dimg[i + 3] = 255
    mean = total / (n * 3.0)
    frac = 100.0 * over / n
    if diff_out:
        write_png(diff_out, wa, ha, bytes(dimg))
    ok = mean < limits["mean"] and frac < limits["frac"]
    text = "mean %.4f/255, %.3f%% px > %d/255, max %d, %d px differ (%.2f%%)" % (
        mean, frac, pix_t, maxd, exact_diff_px, 100.0 * exact_diff_px / n)
    return (ok if limits["tolerant"] else False), text


def main(argv):
    limits = {"tolerant": False, "mean": 1.0, "frac": 0.5, "pix": 8}
    paths = []
    diff = None
    diff_dir = None
    ignore = set()
    i = 0
    while i < len(argv):
        t = argv[i]
        if t == "--tolerant":
            limits["tolerant"] = True
        elif t == "--mean":
            i += 1
            limits["mean"] = float(argv[i])
        elif t == "--frac":
            i += 1
            limits["frac"] = float(argv[i])
        elif t == "--pix":
            i += 1
            limits["pix"] = int(argv[i])
        elif t == "--diff":
            i += 1
            diff = argv[i]
        elif t == "--ignore":
            i += 1
            ignore = set(argv[i].split(","))
        elif t == "--diff-dir":
            i += 1
            diff_dir = argv[i]
        elif t in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            paths.append(t)
        i += 1
    if len(paths) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    a, b = paths
    if os.path.isdir(a) and os.path.isdir(b):
        names = sorted(set(f for f in os.listdir(a) if f.endswith(".png")) | set(f for f in os.listdir(b) if f.endswith(".png")))
        if diff_dir:
            os.makedirs(diff_dir, exist_ok=True)
        bad = 0
        for nme in names:
            base = nme[:-4]
            if base in ignore or (base.endswith("_xr") and base[:-3] in ignore) or (base.endswith("_xr_mv") and base[:-6] in ignore):
                print("%-28s skip" % nme)
                continue
            pa, pb = os.path.join(a, nme), os.path.join(b, nme)
            if not os.path.exists(pa) or not os.path.exists(pb):
                print("%-28s FAIL  missing in %s" % (nme, "A" if not os.path.exists(pa) else "B"))
                bad += 1
                continue
            try:
                ok, text = compare(pa, pb, limits, os.path.join(diff_dir, nme) if diff_dir else None)
            except Exception as e:  # unreadable PNG
                ok, text = False, "error: %s" % e
            if not ok:
                bad += 1
            print("%-28s %s  %s" % (nme, "ok  " if ok else "FAIL", text))
        print("%d of %d differ beyond the %s rule" % (bad, len(names), "tolerant" if limits["tolerant"] else "bit-exact"))
        return 1 if bad else 0
    ok, text = compare(a, b, limits, diff)
    print("%s  %s" % ("ok  " if ok else "FAIL", text))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
