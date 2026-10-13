#!/usr/bin/env python3
"""One table from KartFrame sessions' logs (standard library only).

  frame/tools/session-report.py DIR [DIR ...]          session folders: the game's log and vrtiming.log
  frame/tools/session-report.py LOG [VRTIMING] ...      named logs: the game's log, then its vrtiming.log if any

A session is the game's log (the runtime log, holding the "vr pacing:" lines) and, when present,
the vrtiming.log of the same session (SteamVR's compositor timing, one line a second, written by
frame/tools/vrtiming.py; its format is documented there). In a folder (a frame/kartframe.sh session
folder) the game's log is the *.log other than vrtiming.log with the most pacing lines. Several sessions are columns of the same table, so a baseline and
a later run line up; a mock run (frame/tools/mockrun.sh) has no vrtiming.log.

The game's numbers come from the line it logs every second (runtime/include/vr/openxr_diagnostics.h
documents it; keep the two in step):

  vr pacing: t=<s> hz=<Hz> mode=<inline|pipelined>[/2] ticks=<n> rendered=<n> interpolated=<n>
             late=<n> missed=<n> repeats=<n> gpu_ms=<p50>/<p99> encode_ms=<p50>/<p99>

and from "vr pacing totals:" (same fields, the whole session; its percentiles are exact) and
"vr pacing: ... rendering every second refresh" / "... every refresh again" (half-rate switches).
A hitch is a second whose GPU p99 or encode p99 is above 100 ms, or a compositor client frame
interval above 100 ms.
"""
import os
import re
import sys

HITCH_MS = 100.0
PACING = re.compile(r"vr pacing(?P<tot> totals)?: t=(?P<t>[\d.]+) hz=(?P<hz>[\d.]+) mode=(?P<mode>\S+) ticks=(?P<ticks>\d+) "
                    r"rendered=(?P<rendered>\d+) interpolated=(?P<interp>\d+) late=(?P<late>\d+) missed=(?P<missed>\d+) "
                    r"repeats=(?P<repeats>\d+) gpu_ms=(?P<g50>[\d.]+)/(?P<g99>[\d.]+) encode_ms=(?P<e50>[\d.]+)/(?P<e99>[\d.]+)")


def parse_game_log(path):
    r = {"name": os.path.basename(path), "seconds": 0, "t_last": 0.0, "hz": None, "modes": set(), "ticks": 0, "rendered": 0,
         "interp": 0, "late": 0, "missed": 0, "repeats": 0, "g50": [], "g99": [], "e50": [], "e99": [],
         "totals": None, "half": 0, "full": 0, "hitch": 0, "hitch_notes": [], "refresh_note": None,
         "worst_late": (0, 0), "race": None, "in_race": False,
         "parts": {k: {"seconds": 0, "rendered": 0, "late": 0, "ticks": 0, "g50": [], "g99": [], "e50": []} for k in ("menus", "race")}}
    with open(path, errors="replace") as f:
        for line in f:
            if "entered RaceScene" in line:
                r["in_race"] = True
                if r["race"] is None:
                    r["race"] = r["t_last"]
            elif "exited RaceScene" in line:
                r["in_race"] = False
            m = PACING.search(line)
            if m:
                d = m.groupdict()
                if d["tot"]:
                    r["totals"] = d
                    continue
                r["seconds"] += 1
                r["t_last"] = float(d["t"])
                r["hz"] = d["hz"]
                r["modes"].add(d["mode"])
                for k in ("ticks", "rendered", "interp", "late", "missed", "repeats"):
                    r[k] += int(d[k])
                due = int(d["rendered"]) + int(d["late"])
                if due and int(d["late"]) / due > r["worst_late"][0] / max(r["worst_late"][1], 1):
                    r["worst_late"] = (int(d["late"]), due)
                for k in ("g50", "g99", "e50", "e99"):
                    r[k].append(float(d[k]))
                part = r["parts"]["race" if r["in_race"] else "menus"]
                part["seconds"] += 1
                for k in ("rendered", "late", "ticks"):
                    part[k] += int(d[k])
                for k in ("g50", "g99", "e50"):
                    part[k].append(float(d[k]))
                worst = max(float(d["g99"]), float(d["e99"]))
                if worst > HITCH_MS:
                    r["hitch"] += 1
                    r["hitch_notes"].append("t=%s %.0f ms" % (d["t"], worst))
                continue
            if "rendering every second refresh" in line:
                r["half"] += 1
            elif "rendering every refresh again" in line:
                r["full"] += 1
            g = re.search(r"display refresh rates: (.*?); requested (.*)", line)
            if g:
                r["refresh_note"] = "offered %s, asked %s" % (g.group(1), g.group(2))
    return r


def parse_vrtiming(path):
    r = {"seconds": 0, "frames": 0, "reused": 0, "mis": 0, "dropped": 0, "rp_cpu": 0, "rp_gpu": 0,
         "comp_gpu": [], "interval_max": 0.0, "interval_avg": [], "app_ms_max": 0.0, "hitch": 0, "hitch_notes": [],
         "async": 0, "pids": set()}
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                g = re.search(r"scene app (?:now )?pid (\d+)", line)
                if g:
                    r["pids"].add(g.group(1))
                continue
            g = re.match(r"(\d\d:\d\d:\d\d)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(-?[\d.]+)\s+"
                         r"([\d.]+)/([\d.]+)\s+([\d.]+)/([\d.]+)\s+([\d.]+)", line)
            if not g:
                continue
            v = [float(x) for x in g.groups()[1:]]
            r["seconds"] += 1
            r["frames"] += int(v[0])
            r["reused"] += int(v[1])
            r["mis"] += int(v[2])
            r["dropped"] += int(v[3])
            r["rp_cpu"] += int(v[4])
            r["rp_gpu"] += int(v[5])
            r["async"] += int(v[6])
            r["comp_gpu"].append(v[8])
            r["interval_avg"].append(v[9])
            r["interval_max"] = max(r["interval_max"], v[10])
            r["app_ms_max"] = max(r["app_ms_max"], v[12])
            if v[10] > HITCH_MS:
                r["hitch"] += 1
                r["hitch_notes"].append("%s client interval max %.0f ms" % (g.group(1), v[10]))
    return r


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else None


def fmt(v, spec="%.1f", unit=""):
    return "-" if v is None else (spec % v) + unit


def pct(a, b):
    return "-" if not b else "%.1f%%" % (100.0 * a / b)


def build(sessions):
    """sessions: list of (label, game dict or None, vr dict or None). Returns rows [(label, [cells])]."""
    rows = []

    def add(label, fn):
        cells = []
        for _, g, v in sessions:
            try:
                cells.append(fn(g, v))
            except Exception:
                cells.append("-")
        rows.append((label, cells))

    def tot(g, key):
        return g["totals"][key] if g and g["totals"] else None

    add("duration (min)", lambda g, v: fmt(g["t_last"] / 60.0, "%.1f") if g and g["seconds"] else "-")
    add("display refresh rate (Hz)", lambda g, v: (g["hz"] or "-") if g else "-")
    add("pacing mode(s)", lambda g, v: ",".join(sorted(g["modes"])) or "-" if g else "-")
    add("compositor ticks / s", lambda g, v: fmt(g["ticks"] / g["seconds"]) if g and g["seconds"] else "-")
    add("sets shown / s", lambda g, v: fmt(g["rendered"] / g["seconds"]) if g and g["seconds"] else "-")
    add("  of them interpolated", lambda g, v: pct(g["interp"], g["rendered"]) if g else "-")
    add("late sets", lambda g, v: "%d (%s)" % (g["late"], pct(g["late"], g["rendered"] + g["late"])) if g else "-")
    add("  worst second", lambda g, v: "%d of %d" % g["worst_late"] if g and g["worst_late"][1] else "-")
    add("missed refreshes", lambda g, v: str(g["missed"]) if g else "-")
    add("retained repeats", lambda g, v: "%d (%s of ticks)" % (g["repeats"], pct(g["repeats"], g["ticks"])) if g else "-")
    add("half-rate switches (down / up)", lambda g, v: "%d / %d" % (g["half"], g["full"]) if g else "-")
    add("GPU ms per set p50 / p99 (session)", lambda g, v: "%s / %s" % (tot(g, "g50"), tot(g, "g99")) if tot(g, "g50") else
        "%s / %s" % (fmt(median(g["g50"]), "%.2f"), fmt(max(g["g99"]) if g["g99"] else None, "%.2f")) if g else "-")
    add("encode ms p50 / p99 (session)", lambda g, v: "%s / %s" % (tot(g, "e50"), tot(g, "e99")) if tot(g, "e50") else
        "%s / %s" % (fmt(median(g["e50"]), "%.2f"), fmt(max(g["e99"]) if g["e99"] else None, "%.2f")) if g else "-")
    add("first race at (s)", lambda g, v: fmt(g["race"], "%.0f") if g else "-")
    for name in ("menus", "race"):
        def part(g, name=name):
            return g["parts"][name]
        add("%s: seconds, sets shown / s, late" % name, lambda g, v, part=part: "%d, %s, %s" % (
            part(g)["seconds"], fmt(part(g)["rendered"] / part(g)["seconds"]) if part(g)["seconds"] else "-",
            pct(part(g)["late"], part(g)["rendered"] + part(g)["late"])) if g else "-")
        add("%s: GPU ms p50 (median s) / p99 (worst s), encode p50" % name, lambda g, v, part=part: "%s / %s, %s" % (
            fmt(median(part(g)["g50"]), "%.2f"), fmt(max(part(g)["g99"]) if part(g)["g99"] else None, "%.2f"),
            fmt(median(part(g)["e50"]), "%.2f")) if g else "-")
    add("compositor seconds logged", lambda g, v: str(v["seconds"]) if v else "no vrtiming.log")
    add("compositor frames", lambda g, v: str(v["frames"]) if v else "-")
    add("  app frames reused", lambda g, v: "%d (%s)" % (v["reused"], pct(v["reused"], v["frames"])) if v else "-")
    add("  mispresented", lambda g, v: str(v["mis"]) if v else "-")
    add("  dropped", lambda g, v: str(v["dropped"]) if v else "-")
    add("  reprojected (cpu / gpu flagged)", lambda g, v: "%d / %d" % (v["rp_cpu"], v["rp_gpu"]) if v else "-")
    add("compositor GPU ms avg", lambda g, v: fmt(mean([x for x in v["comp_gpu"] if x >= 0])) if v else "-")
    add("client frame interval: avg / max ms", lambda g, v: "%s / %s" % (fmt(mean(v["interval_avg"]), "%.2f"), fmt(v["interval_max"], "%.1f")) if v else "-")
    add("hitches > %d ms" % HITCH_MS, lambda g, v: str((g["hitch"] if g else 0) + (v["hitch"] if v else 0)))
    return rows


def game_log_in(d):
    """The session's log holding the pacing lines (game.log or stdout.log in a wrapper session)."""
    logs = [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".log") and not f.startswith("vrtiming")]

    def pacing_lines(path):
        with open(path, errors="replace") as f:
            return sum(1 for line in f if "vr pacing" in line)
    return max(logs, key=lambda p: (pacing_lines(p), os.path.getmtime(p))) if logs else None


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if argv else 1
    sessions = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if os.path.isdir(a):
            lp = game_log_in(a)
            vp = os.path.join(a, "vrtiming.log")
            sessions.append((os.path.basename(os.path.normpath(a)), lp, vp if os.path.exists(vp) else None))
        else:
            vp = None
            if i + 1 < len(argv) and "vrt" in os.path.basename(argv[i + 1]):
                vp = argv[i + 1]
                i += 1
            sessions.append((os.path.basename(a), a, vp))
        i += 1
    parsed = [(label, parse_game_log(lp) if lp else None, parse_vrtiming(vp) if vp else None) for label, lp, vp in sessions]
    rows = build(parsed)
    w0 = max(len(r[0]) for r in rows)
    ws = [max(len(parsed[k][0]), max(len(r[1][k]) for r in rows)) for k in range(len(parsed))]
    print("%-*s  %s" % (w0, "", "  ".join("%-*s" % (ws[k], parsed[k][0]) for k in range(len(parsed)))))
    for label, cells in rows:
        print("%-*s  %s" % (w0, label, "  ".join("%-*s" % (ws[k], cells[k]) for k in range(len(cells)))))
    notes = []
    for label, g, v in parsed:
        for src, d in (("game log", g), ("compositor", v)):
            if d and d["hitch_notes"]:
                notes.append("%s (%s): %s" % (label, src, "; ".join(d["hitch_notes"][:8]) + (" ..." if len(d["hitch_notes"]) > 8 else "")))
        if g and g["refresh_note"]:
            notes.append("%s: refresh rates %s" % (label, g["refresh_note"]))
    if notes:
        print()
        print("\n".join(notes))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
