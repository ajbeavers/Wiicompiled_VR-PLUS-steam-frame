# Lane X: session 19 frame by frame, and the end lock against the compositor's grid

Data: session 19, `$KARTFRAME_HOME/sessions/20261016-012613` (launch 1, owner: doubled in the right eye) and
`20261016-012746` (launch 2, owner: clean, right eye "more sensitive to movement"), both
`KARTFRAME_END_DELAY_US=4000`, race only (33.6 s / 36.5 s, 4022 / 4355 compositor frames). Tool:
`python3 -I frame/tools/phase.py DIR` (view C below is new in this lane).

## 1. Method

All window numbers are in SteamVR's own clock (no clock matching): compositor frame k reports our frame-ready
(our xrEndFrame as SteamVR saw it, about 0.12 ms after our call) relative to its own vsync, normally -3.5 ms,
i.e. in the previous refresh. So our end lands at `fr(k) + (sys(k) - sys(k-1))` after vsync k-1, and frame
k-1's update start / end are the compositor window it falls in. Frames whose compositor logged no update times
(start = end = 0: 140 / 42 frames, around compositor hiccups) are left out. Our release (XRF r=) is 0.05 ms
before the end (p50; the doubled launch has a 0.55 ms p95 between release and end, the clean one 0.10), so
release and end classify the same.

Clock note: SteamVR's m_flSystemTimeInSeconds runs on CLOCK_MONOTONIC_RAW (the vsync from
GetTimeSinceLastVsync in _RAW minus the frame's system time is constant to +-10 us, folded to one refresh);
CLOCK_MONOTONIC drifts 14 / 33 ppm against it in these launches. phase.py now matches through _RAW; its
per-poll matching already followed the drift, so view B's numbers did not change. Fixed: view B's "end vs frame
ready" check used only positive frame-ready values (it now reads -0.12 ms p50, i.e. the clocks agree).

## 2. Where the end lands (view C)

| race frames | doubled 012613 | clean 012746 |
| --- | --- | --- |
| end after vsync, p5 / p50 / p95 ms | 4.52 / 4.81 / 5.67 | 4.56 / 4.85 / 5.42 |
| update start, p5 / p50 / p95 | 0.22 / 2.83 / 3.17 | 0.62 / 3.03 / 3.20 |
| update end, p5 / p50 / p95 | 3.37 / 5.25 / 5.49 | 4.74 / 5.32 / 5.50 |
| end BEFORE its refresh's update start | **0** | **0** |
| end INSIDE the update window | 3021 (77.7 %) | 3824 (88.6 %) |
| end AFTER the update end | 865 (22.3 %) | 492 (11.4 %) |
| end minus update end, p5 / p50 / p95 | -0.85 / -0.37 / +1.04 | -0.82 / -0.42 / +0.34 |
| class changes between consecutive refreshes | **975 (25.1 %)** | **548 (12.7 %)** |
| the same, seconds without an early update start | 25.0 % (after 16.0 %) | 11.6 % (after 8.7 %) |

SteamVR's per-frame presents by class (presented 0x / 2x+ / mispresented):

| class | doubled 012613 | clean 012746 |
| --- | --- | --- |
| inside | 46 / 25 / 52 of 3021 (2.4 % not 1x) | 17 / 10 / 20 of 3824 (0.7 %) |
| after | 69 / 39 / 121 of 865 (12.5 %) | 22 / 20 / 42 of 492 (8.5 %) |

Anomalies against end minus update end (doubled launch; the clean one has the same shape at lower rates):
-1.0 to 0 ms: 1.4-3.1 %; 0 to +0.25: 4.2 %; +0.25 to +1.25: 11-28 %; later than +1.5 (hitches): 50 %.

## 3. The update window over time

The early update start is neither random nor a steady beat: it comes in **episodes**. Each begins with a
compositor discontinuity (one frame presented 2-3 times or mispresented, then one or two frames with no update
times, sometimes an update a whole refresh late at +8.6 ms), after which the update starts 0.2-1.4 ms after the
vsync and ends early too (3.2-4.0 ms; in the clean launch's end-of-race episode 4.1-5.6), and creeps back to
about +2.9 over 0.3 s (or 3 s):

| launch | episodes (race time, length) |
| --- | --- |
| doubled 012613 | 4.7 s (0.31 s), 9.7 s (0.34 s, after a late update), 15.7 s (0.28 s), 29.6 s (3.2 s) |
| clean 012746 | 32.9 s (1.5 s), 34.4 s (1.7 s) (the race's last seconds) |

The doubled launch's three short ones are 5.0 and 6.0 s apart (a beat-like recurrence of compositor hiccups,
cause unknown: a load or scheduling effect on vrcompositor, not our end); its seconds 22-29 also had the whole
window 0.2-0.5 ms earlier (update start p50 2.4, end 4.8-5.0) with 22-58 "after" frames a second. Outside the
episodes the window is steady at start 2.8-3.2, end 5.2-5.4.

## 4. The model, checked

The orchestrator's reading was "our release fell about 3 ms BEFORE the update in at least 5 % of the doubled
launch's frames". **The per-frame data contradicts that part: no end in either launch landed before its
refresh's update start.** The -3 ms in view B was the nearest-anchor fold: during an episode the update starts
about 4.2 ms before our release, past the +-4.17 ms fold, so the nearest start was the next refresh's. In the
early-start frames our end lands AFTER the (early) update end instead.

What the data supports: the end sits 0.4 ms before the compositor's **update end** in both launches (p50
-0.37 / -0.42), so jitter on either side (our end's tail: p95 - p50 = 0.6-0.9 ms; the update end's: p50 - p5 =
0.6-1.9 ms) carries it across the update end in some refreshes and not in others. The doubled launch crossed
twice as often (25 % vs 13 % class changes, also outside the episodes: 25 % vs 12 %), with its update end
earlier and its own end later in the tails, plus four re-phase episodes. Frames that land after the update end
are the ones SteamVR itself presents 0 or 2 times or mispresents (5-12x the rate of inside frames). With the
per-eye pick-up model (each eye takes the latest released image when the compositor binds it, the right eye
late in the update): inside = the right eye takes the new set a refresh early (consistently: "right eye more
sensitive"), after = both eyes take it on time; every change between the two repeats or skips a set in the right
eye only. That matches the right eye doubling and the left staying clean, and the 2:1 ratio of changes matches
doubled vs clean.

What it does not explain (earlier sessions, no compositor log): session 17's 6000 us launch ended about vsync +
6.9 (1.6 ms after the update end, every frame) and was "right eye very bad, left nearly perfect with a 2 s beat";
session 18's 3500 / 4500 us launches (about vsync + 4.0 / + 5.0, inside the window) were doubled in the right
eye. If the window was where session 19 shows it, the right eye has a second sensitive point after the update
end (e.g. where the compositor's GPU pass reads it, 1-2 ms after the CPU update) and the clean band is narrow
on both sides. Session 20 logs the compositor in every launch, so each verdict gets its own window.

## 5. Consequences for the lock

- The medians did not differ between the clean and the doubled launch (wake-up +0.34 / +0.42 after the vsync,
  end +4.81 / +4.85, update end +5.25 / +5.32): a lock that only tracks the median grid would have produced
  the same two launches. What the lock must give is **margin from the update end** in the tails, held while
  the compositor re-phases.
- "before" (update start p50 - 1.5 ms, about vsync + 1.5, about 1.1 ms after the wake-up) lands inside the
  early windows of the episodes (start 0.2-1.4), and is close to the ends at the wake-up (vsync + 0.7) of the
  doubled sessions 4-15. It is the probe of the model, not the expected fix.
- "after" (update end p95 over the last second + 0.3 ms, about vsync + 5.8, about 5.4 ms after the wake-up)
  clears the update end in at least 95 % of refreshes by construction and moves with an episode's window. Its
  open risk is the session 17 point (vsync + 6.9) 1.1 ms later.

## 6. The lock (built, default off)

Helper: frame/tools/vrtiming.py, when `KARTFRAME_END_PHASE` is set in its environment (the wrapper exports the
session.env lines to it), polls every 0.1 s instead of 0.25 s and publishes a 64-byte record (format in
vrtiming.py and openxr_integration.cpp, `EndPhaseGrid`) by atomic rename to `KARTFRAME_PHASE_FILE`, default
`$XDG_RUNTIME_DIR/kartframe-phase.bin`: the newest vsync in CLOCK_MONOTONIC ns (GetTimeSinceLastVsync, read
between two CLOCK_MONOTONIC reads), the refresh period (from the vsyncs of successive polls), update start
p5 / p50 and update end p50 / p95 over the last 120 compositor frames (no-update-time frames left out), and
the write time.
Game: the pacing thread re-reads the file at most every 50 ms (open, pread 64 bytes, close; about 10 us) and
aims the end at a phase of the vsync at or before the wake-up (1 ms slack):
- `KARTFRAME_END_PHASE=after[:us]` -> vsync + update end p95 + us (default 300),
- `KARTFRAME_END_PHASE=before[:us]` -> vsync + update start p50 - us (default 1500),
bounded to [now, wake-up + period - pacing margin]. It falls back to `KARTFRAME_END_DELAY_US` (or 4000 us, the
session 16/19 setting, when unset) whenever the record is missing, older than 0.5 s, or its period is more than
1 % off xrWaitFrame's, and logs `end phase:` once for the mode, at every switch between lock and fallback, and
every 10 s the achieved phase (p50 / p95 of the end minus the published update start and update end, the end
after the wake-up, and the share of fallback frames). The order is lane V's: the next set (locate, acquire,
encode) starts at the wake-up before the sleep, 4 eye pairs; if Aurora was busy and the set starts after the
end, in "before" mode that start waits until the published update end + 0.3 ms (bounded by the cycle's
deadline) so no acquire or locate lands in the window; both eyes are released back to back in the end call.

Headless check, results and session 20: frame/perf/session-h20/README.md.
