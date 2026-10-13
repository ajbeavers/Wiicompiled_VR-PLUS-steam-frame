#!/usr/bin/env python3
"""Game-frame hitches of a run: hitches.py RUN_DIR [RACE_START_WINDOW_S] (lane E).

Reads aurora's "hitch:" lines (one per game frame longer than 20 ms, end_frame to end_frame, with what
the process did meanwhile; aurora-main/lib/gfx/hitch_stats.hpp), "vr pacing clock" (the session start
on CLOCK_MONOTONIC), the pacing seconds around "entered/exited RaceScene", and io.tsv (iosample.py:
read bytes and newly opened game files every 50 ms). Prints histograms for menus, the race and the
first N seconds (10) of each race, and the frames with their likely cause:
  pipelines   blocked on or compiling pipelines (pipeline_wait + inline compiles) for most of the excess
  textures    CPU texture conversion for most of the excess
  disc        game data read in the frame (rchar, newly opened files under DATA/files)
  worker      waiting for the frame worker (Aurora's encode of the previous frame) or its GPU lock
  no cpu      the game thread was runnable but waiting for a CPU (runq_ms) for most of the excess
  aurora begin  inside aurora_begin_frame (frame-worker preparation) for most of the excess
  game cpu    the game thread computed for 80 % of a 60 Hz frame or more (game logic, GX recording)
  blocked     none of these: the thread slept elsewhere (the game's own waits: VI retrace, threads)
  other       older logs without game_cpu_ms: none of the above."""
import bisect
import os
import re
import sys

CAUSES = ("pipelines", "textures", "disc", "worker", "no cpu", "aurora begin", "game cpu", "blocked", "other")
BUCKETS = [(20, 33), (33, 50), (50, 100), (100, 250), (250, 1000), (1000, 1e9)]
HITCH = re.compile(r"hitch: mono=([\d.]+) frame_ms=([\d.]+)(?: game_cpu_ms=([\d.]+))?(?: runq_ms=([\d.]+) begin_ms=([\d.]+))? worker_wait_ms=([\d.]+) pipelines_new=(\d+) pipeline_wait_ms=([\d.]+) "
                   r"pipeline_inline=(\d+) pipeline_inline_ms=([\d.]+) tex_convert_ms=([\d.]+) tex_convert_kb=(\d+) tex_upload_kb=(\d+)(?: begin_joins=(\d+))?")


def load(d):
    hitches, races, start = [], [], None
    t_last, race_open = 0, None
    with open(os.path.join(d, "game.log"), errors="replace") as f:
        for line in f:
            m = re.search(r"vr pacing clock: session start mono=([\d.]+)", line)
            if m and start is None:
                start = float(m.group(1))
            m = re.search(r"vr pacing: t=(\d+) ", line)
            if m:
                t_last = int(m.group(1))
            if "entered RaceScene" in line:
                race_open = t_last
            elif "exited RaceScene" in line and race_open is not None:
                races.append((race_open, t_last + 1))
                race_open = None
            m = HITCH.search(line)
            if m:
                v = [float(x) if x is not None else -1.0 for x in m.groups()]
                hitches.append(dict(zip(("mono", "ms", "cpu", "runq", "begin", "worker", "pnew", "pwait", "pinl", "pinlms", "tex", "texkb", "upkb", "joins"), v)))
    if race_open is not None:
        races.append((race_open, 1e9))
    io = []
    p = os.path.join(d, "io.tsv")
    if os.path.exists(p):
        for line in open(p):
            if line.startswith("#"):
                continue
            c = line.rstrip("\n").split("\t")
            io.append((float(c[0]), int(c[1]), int(c[2]), c[3] if len(c) > 3 else ""))
    return hitches, races, start, io


def io_between(io, a, b):
    """Bytes read and files opened between monotonic times a and b (50 ms samples, so widened by one)."""
    if not io:
        return 0, []
    times = [x[0] for x in io]
    i = max(bisect.bisect_left(times, a) - 1, 0)
    j = min(bisect.bisect_right(times, b), len(io) - 1)
    files = [f for x in io[i + 1:j + 1] for f in x[3].split(",") if f]
    return io[j][1] - io[i][1], files


def cause(h, rbytes, files):
    excess = h["ms"] - 16.7
    pipe = h["pwait"] + h["pinlms"]
    if files or rbytes > 256 * 1024:
        return "disc"
    if pipe >= 0.5 * excess:
        return "pipelines"
    if h["tex"] >= 0.5 * excess:
        return "textures"
    if h["worker"] >= 0.5 * excess:
        return "worker"
    if h["runq"] >= 0.5 * excess:
        return "no cpu"
    if h["begin"] >= 0.5 * excess:
        return "aurora begin"
    if h["cpu"] >= 0.8 * 16.7:
        return "game cpu"
    return "blocked" if h["cpu"] >= 0 else "other"


def main(d, window, limit=60):
    hitches, races, start, io = load(d)
    if start is None:
        print("no 'vr pacing clock' line; is this a run of a build with lane E's diagnostics?")
        return 1
    groups = {"menus": [], "race": [], "race start (first %ds)" % window: []}
    rows = []
    for h in hitches:
        t = h["mono"] - start
        part = "menus"
        for a, b in races:
            if a <= t < b:
                part = "race start (first %ds)" % window if t - a < window else "race"
        rb, files = io_between(io, h["mono"] - h["ms"] / 1000.0, h["mono"])
        c = cause(h, rb, files)
        groups[part].append((h, c))
        rows.append((t, part, h, c, rb, files))
    print("races at session seconds: %s" % ", ".join("%d-%s" % (a, "end" if b >= 1e9 else int(b)) for a, b in races))
    print()
    print("| part | frames > 20 ms | " + " | ".join("%d-%s ms" % (a, "" if b >= 1e9 else int(b)) for a, b in BUCKETS) +
          " | " + " | ".join(CAUSES) + " |")
    print("| --- " * (len(BUCKETS) + 2 + len(CAUSES)) + "|")
    for name, items in groups.items():
        hist = [sum(1 for h, _ in items if a <= h["ms"] < b) for a, b in BUCKETS]
        causes = [sum(1 for _, c in items if c == k) for k in CAUSES]
        print("| %s | %d | %s | %s |" % (name, len(items), " | ".join(map(str, hist)), " | ".join(map(str, causes))))
    print()
    print("frames over 33 ms (t = session seconds):")
    shown = 0
    for t, part, h, c, rb, files in rows:
        if h["ms"] >= 33 and shown < limit:
            shown += 1
            print("  t=%.1f %s %.0f ms (cpu %.0f, runq %.0f, begin %.0f): %s; worker %.1f, pipelines new %d wait %.1f inline %d/%.1f ms, tex %.1f ms %d KiB, "
                  "upload %d KiB, read %d KiB%s" % (t, part, h["ms"], h["cpu"], h["runq"], h["begin"], c, h["worker"], h["pnew"], h["pwait"], h["pinl"], h["pinlms"],
                                                    h["tex"], h["texkb"], h["upkb"], rb // 1024,
                                                    (", opened " + " ".join(files[:4])) if files else ""))
    return 0


if __name__ == "__main__":
    if not sys.argv[1:]:
        print(__doc__)
        sys.exit(1)
    sys.exit(main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 10))
