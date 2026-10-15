# Headset session 15: GPU load from both sides (90 Hz at 1728; 120 Hz at 1200 px)

Two launches after session 14 (desktop present and release backlog ruled out; a drained queue cleared the
left eye only): `01-90hz.env` (KARTFRAME_REFRESH_HZ=90) and `02-eye1200.env` (new wrapper knob
KARTFRAME_EYE_PX=1200, measurement only). Both settings took effect (read-back 5 of 5; 90 Hz; 1200x1200
eye swapchains). Pacing: 90.7 sets/s and 120.5 sets/s, 0.0 % late in both; GPU per set 5.2 ms at 90 Hz
(1728) and 3.1 ms at 1200 px; ends > 1 ms 3.5 % and 3.1 %.

Owner: "bad again for both, but the second launch had less doubling with the right eye".

Reading: neither the refresh period (11.1 ms) nor halving the GPU work removes the doubling, so the SIZE of
our GPU work and the refresh rate are not the cause. Two default launches played by mistake just before
(no session file) doubled as always. What is left between the clean probe (session 13) and the game: the
game process itself around the hand-over, i.e. its threads. The priority boost (nice -15, 673408a) and the
utilisation clamp (4b87fcf) both landed after session 3's build (ff9b277, the only clean session) and were
in every doubled session since; session 16 turns them off (`session-h16/01-noprio.env`).
