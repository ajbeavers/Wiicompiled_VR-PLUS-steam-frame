#!/usr/bin/env python3
"""Where a set's GPU time goes, by part of the demo cycle: gpuprof.py RUN_DIR [FIRST_S LAST_S] (lane I).

Reads the "GPU ms per game-frame sets / refresh sets" lines KARTFRAME_FPSLOG=2 writes every 5 s (in-pass
timestamps: gfx::gpu_timing_mark) and averages each segment per set over a part (menus, race 1p/2p/4p,
pacing-parts.py's rule) or over the pacing seconds FIRST_S..LAST_S. Prints a markdown table: ms per set
for game-frame sets (native render + an eye set) and refresh sets (an eye set alone), plus the span
p50/p99 (first to last timestamp of the set's GPU work)."""
import re
import sys

LINE = re.compile(r"GPU ms per (game-frame sets|refresh sets) \((\d+)\): span p50/p99 ([\d.]+)/([\d.]+)((?: [\w-]+=[\d.]+)*)")
PACING = re.compile(r"vr pacing: t=(\d+) ")


def main():
    d = sys.argv[1]
    window = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else None
    parts = {}
    screens, in_race, t = 1, False, 0
    for line in open(f"{d}/game.log", errors="replace"):
        m = re.search(r"presentation=immersive-race, scene=\d+, screens=(\d+)", line)
        if m:
            screens = int(m.group(1))
        if "entered RaceScene" in line or "presentation=immersive-race" in line:
            in_race = True
        elif "exited RaceScene" in line or "presentation=virtual-screen" in line or "presentation=desktop" in line:
            in_race = False
        m = PACING.search(line)
        if m:
            t = int(m.group(1))
        for chunk in line.split("; "):
            m = LINE.search(chunk)
            if not m:
                continue
            if window:
                if not window[0] <= t <= window[1]:
                    continue
                part = f"t={window[0]}-{window[1]}"
            else:
                part = "menus" if not in_race else ("race 1p" if screens <= 1 else f"race {screens}p")
            kind, n = m.group(1), int(m.group(2))
            p = parts.setdefault((part, kind), {"n": 0, "seg": {}, "p50": [], "p99": []})
            p["n"] += n
            p["p50"].append(float(m.group(3)))
            p["p99"].append(float(m.group(4)))
            for kv in m.group(5).split():
                k, v = kv.split("=")
                p["seg"][k] = p["seg"].get(k, 0.0) + float(v) * n
    keys = []
    for p in parts.values():
        for k in p["seg"]:
            if k not in keys:
                keys.append(k)
    print("| part | sets | sets timed | span p50 | span p99 (worst 5 s) | sum | " + " | ".join(keys) + " |")
    print("| --- | --- | --- | --- | --- | --- | " + " | ".join("---" for _ in keys) + " |")
    for (part, kind), p in sorted(parts.items()):
        n = max(p["n"], 1)
        segs = [p["seg"].get(k, 0.0) / n for k in keys]
        mid = sorted(p["p50"])[len(p["p50"]) // 2]
        print(f"| {part} | {kind} | {p['n']} | {mid:.2f} | {max(p['p99']):.2f} | {sum(segs):.2f} | "
              + " | ".join(f"{v:.2f}" for v in segs) + " |")


main()
