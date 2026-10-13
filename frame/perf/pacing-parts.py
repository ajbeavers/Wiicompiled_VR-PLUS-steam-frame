#!/usr/bin/env python3
"""Menus and race apart, with exact percentiles: pacing-parts.py RUN_DIR [RUN_DIR ...] (lane E).

A run dir is frame/perf/run.sh's output: game.log (the "vr pacing:" lines, "entered/exited
RaceScene", the mock's summary) and samples.txt (KARTFRAME_PACING_SAMPLES: every set's GPU and
encode ms per pacing second). A second belongs to the race from the pacing line after "entered
RaceScene" up to the one after "exited RaceScene" (session-report.py's rule). Prints one markdown row
per run and part: sets/s, late % (of sets due), repeats % (of ticks), missed refreshes, half-rate
switches, GPU ms per set p50/p99, encode ms p50/p99, game frames/s (KARTFRAME_FPSLOG lines), then the mock's own counts against the game's."""
import os
import re
import sys

PACING = re.compile(r"vr pacing: t=(\d+) hz=([\d.]+) mode=(\S+) ticks=(\d+) rendered=(\d+) interpolated=(\d+) "
                    r"late=(\d+) missed=(\d+) repeats=(\d+)")
TOTALS = re.compile(r"vr pacing totals: .* ticks=(\d+) rendered=(\d+) interpolated=(\d+) late=(\d+) missed=(\d+) repeats=(\d+)")
FPS = re.compile(r"Game frame rate [\d.]+ FPS \((\d+) frames in ([\d.]+) s\); per frame the producer waited ([\d.]+) ms for DONE")
MOCK = re.compile(r"xrmock: frames: (\d+) ended; (\d+) showed newly released eyes .*?\), (\d+) repeated a projection layer, "
                  r"(\d+) a quad, (\d+) no layer")


def pct(values, p):
    if not values:
        return None
    v = sorted(values)
    return v[min(len(v) - 1, int(p * (len(v) - 1) + 0.5))]


def race_part(in_race, screens):
    if not in_race:
        return "menus"
    return "race" if screens <= 1 else ("race 2p" if screens == 2 else "race 4p")


def run(d):
    # The attract-mode demo alternates 1, 2 and 4 player races (MKW runs 3 and 4 players at 30 fps):
    # "race" is the 1-player race (the owner's case), "race Np" the split-screen ones.
    parts = {k: {"s": 0, "ticks": 0, "rendered": 0, "late": 0, "missed": 0, "repeats": 0, "gpu": [], "enc": [], "half": 0,
                 "frames": 0, "fsecs": 0.0, "done_ms": 0.0}
             for k in ("menus", "race", "race 2p", "race 4p")}
    screens = 1
    second_part = {}
    in_race = False
    totals = mock = None
    half = []
    with open(os.path.join(d, "game.log"), errors="replace") as f:
        for line in f:
            m = re.search(r"presentation=immersive-race, scene=\d+, screens=(\d+)", line)
            if m:
                screens = int(m.group(1))
            if "entered RaceScene" in line or "presentation=immersive-race" in line:
                in_race = True
            elif "exited RaceScene" in line or "presentation=virtual-screen" in line or "presentation=desktop" in line:
                in_race = False
            elif "rendering every second refresh" in line:
                half.append(("down", in_race))
            elif "rendering every refresh again" in line:
                half.append(("up", in_race))
            m = PACING.search(line)
            if m:
                t = int(m.group(1))
                p = parts[race_part(in_race, screens)]
                second_part[t] = p
                p["s"] += 1
                for i, k in ((4, "ticks"), (5, "rendered"), (7, "late"), (8, "missed"), (9, "repeats")):
                    p[k] += int(m.group(i))
                continue
            m = FPS.search(line)
            if m:
                p = parts[race_part(in_race, screens)]
                p["frames"] += int(m.group(1))
                p["fsecs"] += float(m.group(2))
                p["done_ms"] += float(m.group(3)) * int(m.group(1))
                continue
            m = TOTALS.search(line)
            if m:
                totals = [int(x) for x in m.groups()]
            m = MOCK.search(line)
            if m:
                mock = [int(x) for x in m.groups()]
    sp = os.path.join(d, "samples.txt")
    if os.path.exists(sp):
        for line in open(sp):
            g = re.match(r"t=(\d+) gpu=(\S*) encode=(\S*)", line)
            if not g or int(g.group(1)) not in second_part:
                continue
            p = second_part[int(g.group(1))]
            p["gpu"] += [float(x) for x in g.group(2).split(",") if x]
            p["enc"] += [float(x) for x in g.group(3).split(",") if x]
    return parts, totals, mock, half


def fmt(x):
    return "-" if x is None else "%.2f" % x


def main(dirs):
    print("| run | part | s | sets/s | late % | repeats % | missed | GPU ms p50 / p99 | encode ms p50 / p99 | game fps |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    checks = []
    for d in dirs:
        name = os.path.basename(os.path.normpath(d))
        parts, totals, mock, half = run(d)
        for k in ("menus", "race", "race 2p", "race 4p"):
            p = parts[k]
            if not p["s"]:
                continue
            due = p["rendered"] + p["late"]
            fps = "%.1f" % (p["frames"] / p["fsecs"]) if p["fsecs"] else "-"
            print("| %s | %s | %d | %.1f | %.2f | %.2f | %d | %s / %s | %s / %s | %s |" % (
                name, k, p["s"], p["rendered"] / p["s"], 100.0 * p["late"] / max(due, 1),
                100.0 * p["repeats"] / max(p["ticks"], 1), p["missed"], fmt(pct(p["gpu"], .5)), fmt(pct(p["gpu"], .99)),
                fmt(pct(p["enc"], .5)), fmt(pct(p["enc"], .99)), fps))
        hs = ", ".join("%s in %s" % (a, "race" if r else "menus") for a, r in half) or "none"
        if totals and mock:
            ticks, rendered, interp, late, missed, repeats = totals
            ended, fresh, rep, quad, empty = mock
            checks.append("%s: half-rate switches: %s. game: ticks %d, rendered %d, late %d, missed %d, repeats %d; mock: ended %d, "
                          "fresh %d, repeated layer %d, no layer %d (repeated + no layer = %d)" % (
                              name, hs, ticks, rendered, late, missed, repeats, ended, fresh, rep, empty, rep + empty))
        else:
            checks.append("%s: half-rate switches: %s; no totals or mock summary in the log" % (name, hs))
    print()
    for c in checks:
        print("- " + c)


if __name__ == "__main__":
    if not sys.argv[1:]:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1:])
