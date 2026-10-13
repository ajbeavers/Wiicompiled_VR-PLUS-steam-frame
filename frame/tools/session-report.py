#!/usr/bin/env python3
"""One table from a GalaxyFrame session's logs on the Steam Frame (standard library only).

  tools/session-report.py                       current and previous session in ~/.local/share/GalaxyFrame
  tools/session-report.py DIR                   the same for another data folder ($GALAXYFRAME_HOME)
  tools/session-report.py LOG [VRTIMING] ...    named logs: petari_log.txt, then its vrtiming.log if any

A session is a petari_log.txt (the game's log) and, when present, the vrtiming.log of the
same session (SteamVR's compositor timing, written by steam-frame/galaxyframe.sh through
tools/vrtiming.py).  Folder mode pairs petari_log.txt with vrtiming.log and petari_log.txt.prev
with vrtiming.log.prev.  Several sessions are columns of the same table, so a baseline and a
later run line up.  Columns for logs that are missing are left out of the VR compositor rows.

The game's own numbers come from lines it logs every 10 seconds ("vr: N of M game frames
shown twice", "vr: the frame loop missed ...", "vr: render scale ...; eye GPU ...", "vr: late
first step", "perf: game ... work ..."); the compositor's from vrtiming.log (one line a second).
A hitch is a moment over 100 ms: a late first step, a game work maximum or an eye GPU maximum
above 100 ms in the game's log, a client frame interval above 100 ms in the compositor's.
"""
import os
import re
import sys

HITCH_MS = 100.0


def secs(stamp):
    """HH:MM:SS.mmm to seconds."""
    h, m, s = stamp.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def parse_game_log(path):
    r = {"name": os.path.basename(path), "shown_twice": 0, "frames": 0, "worst_twice": (0, 0),
         "missed": 0, "missed_own": 0, "sets_late": 0, "sets": 0, "first_step": [],
         "eye_avg": [], "eye_max": 0.0, "eye_budget": None, "scale": None,
         "app_gpu_avg": [], "app_gpu_max": 0.0, "game_work_max": 0.0, "game_fps": [],
         "hitch": 0, "hitch_notes": [], "refresh": None, "refresh_note": None,
         "first": None, "last": None, "scale_changes": 0, "refreshes_missed_scale": 0}
    day = 0
    prev = None
    with open(path, errors="replace") as f:
        for line in f:
            m = re.match(r"(\d\d:\d\d:\d\d\.\d+) (.*)", line)
            if not m:
                continue
            t = secs(m.group(1))
            if prev is not None and t < prev - 3600:
                day += 1
            prev = t
            t += day * 86400
            if r["first"] is None:
                r["first"] = t
            r["last"] = t
            msg = m.group(2)
            g = re.match(r"vr: (\d+) of (\d+) game frames shown twice", msg)
            if g:
                a, b = int(g.group(1)), int(g.group(2))
                r["shown_twice"] += a
                r["frames"] += b
                if b and a / b > r["worst_twice"][0] / max(r["worst_twice"][1], 1):
                    r["worst_twice"] = (a, b)
                continue
            g = re.match(r"vr: the frame loop missed (\d+) refreshes in the last [\d.]+ s \((\d+) with its own work late\); "
                         r"(\d+) of (\d+) sets not finished when due", msg)
            if g:
                r["missed"] += int(g.group(1))
                r["missed_own"] += int(g.group(2))
                r["sets_late"] += int(g.group(3))
                r["sets"] += int(g.group(4))
                continue
            g = re.match(r"vr: late first step: ([\d.]+) ms", msg)
            if g:
                v = float(g.group(1))
                r["first_step"].append(v)
                if v > HITCH_MS:
                    r["hitch"] += 1
                    r["hitch_notes"].append("%s late first step %.0f ms" % (m.group(1)[:8], v))
                continue
            g = re.match(r"vr: render scale ([\d.]+)-([\d.]+) \(now [\d.]+, (\d+) changes\); eye GPU ([\d.]+) ms avg ([\d.]+) max of ([\d.]+)", msg)
            if g:
                r["eye_avg"].append(float(g.group(4)))
                mx = float(g.group(5))
                r["eye_max"] = max(r["eye_max"], mx)
                r["eye_budget"] = float(g.group(6))
                r["scale_changes"] += int(g.group(3))
                if mx > HITCH_MS:
                    r["hitch"] += 1
                    r["hitch_notes"].append("%s eye GPU max %.0f ms" % (m.group(1)[:8], mx))
                continue
            g = re.match(r"vr: runtime counters: app GPU ([\d.]+) ms a frame avg ([\d.]+) max", msg)
            if g:
                r["app_gpu_avg"].append(float(g.group(1)))
                r["app_gpu_max"] = max(r["app_gpu_max"], float(g.group(2)))
                continue
            g = re.match(r"perf: game ([\d.]+) fps, work ([\d.]+) ms avg ([\d.]+) max", msg)
            if g:
                r["game_fps"].append(float(g.group(1)))
                mx = float(g.group(3))
                r["game_work_max"] = max(r["game_work_max"], mx)
                if mx > HITCH_MS:
                    r["hitch"] += 1
                    r["hitch_notes"].append("%s game work max %.0f ms" % (m.group(1)[:8], mx))
                continue
            g = re.match(r"display refresh rate now ([\d.]+) Hz", msg)
            if g:
                r["refresh"] = g.group(1)
                continue
            g = re.match(r"display refresh rates: (.*?); requested ([\d.]+) Hz", msg)
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

    add("duration (min)", lambda g, v: fmt((g["last"] - g["first"]) / 60.0, "%.1f") if g and g["first"] is not None else "-")
    add("display refresh rate (Hz)", lambda g, v: (g["refresh"] or "-") if g else "-")
    add("game frames logged", lambda g, v: str(g["frames"]) if g else "-")
    add("frames shown twice", lambda g, v: "%d (%s)" % (g["shown_twice"], pct(g["shown_twice"], g["frames"])) if g else "-")
    add("  worst 10 s window", lambda g, v: "%d of %d" % g["worst_twice"] if g and g["frames"] else "-")
    add("missed refreshes", lambda g, v: "%d (%d own work late)" % (g["missed"], g["missed_own"]) if g else "-")
    add("sets not finished when due", lambda g, v: "%d of %d (%s)" % (g["sets_late"], g["sets"], pct(g["sets_late"], g["sets"])) if g else "-")
    add("late first step: count / avg / max ms", lambda g, v: "%d / %s / %s" % (len(g["first_step"]), fmt(mean(g["first_step"])),
                                                                              fmt(max(g["first_step"]) if g["first_step"] else None)) if g else "-")
    add("eye GPU ms: avg of 10 s avgs / max (budget)", lambda g, v: "%s / %s (%s)" % (fmt(mean(g["eye_avg"])), fmt(g["eye_max"] if g["eye_avg"] else None),
                                                                                      fmt(g["eye_budget"])) if g else "-")
    add("render scale changes", lambda g, v: str(g["scale_changes"]) if g else "-")
    add("runtime app GPU ms/frame: avg / max", lambda g, v: "%s / %s" % (fmt(mean(g["app_gpu_avg"])), fmt(g["app_gpu_max"] if g["app_gpu_avg"] else None)) if g else "-")
    add("game work max (ms)", lambda g, v: fmt(g["game_work_max"] if g["game_fps"] else None) if g else "-")
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


def main(argv):
    if argv and argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    sessions = []
    if not argv or (len(argv) == 1 and os.path.isdir(argv[0])):
        d = argv[0] if argv else os.environ.get("GALAXYFRAME_HOME") or os.environ.get("GALAXYQUEST_HOME") or os.path.expanduser("~/.local/share/GalaxyFrame")
        for label, lg, vt in (("current", "petari_log.txt", "vrtiming.log"), ("previous", "petari_log.txt.prev", "vrtiming.log.prev")):
            lp, vp = os.path.join(d, lg), os.path.join(d, vt)
            if os.path.exists(lp) or os.path.exists(vp):
                sessions.append((label, lp if os.path.exists(lp) else None, vp if os.path.exists(vp) else None))
        perf = os.path.join(d, "perfcriteria.txt")
    else:
        perf = None
        i = 0
        while i < len(argv):
            lp = argv[i]
            vp = None
            if i + 1 < len(argv) and "vrt" in os.path.basename(argv[i + 1]):
                vp = argv[i + 1]
                i += 1
            sessions.append((os.path.basename(lp), lp, vp))
            i += 1
    if not sessions:
        print("no logs found", file=sys.stderr)
        return 1
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
    if perf and os.path.exists(perf):
        notes.append("perfcriteria.txt is in the data folder (Steam's own log for the last session): %s" % perf)
    if notes:
        print()
        print("\n".join(notes))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
