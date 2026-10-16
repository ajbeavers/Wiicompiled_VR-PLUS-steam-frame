# Headset session 17: the end-delay window's edges (2.5 ms and 6 ms)

Same build as session 16. Owner: "both better, but both had doubling in both eyes; launch 1 (2500 us) left
eye okay with slight doubling; launch 2 (6000 us) left eye damn near perfect, doubling slightly for half a
second every 2 seconds; in both the right eye was very bad."

Logs: launch 1's ends were NOT at 2.5 ms: XRF e 2-3 ms 3688 frames, 3-4 ms 1184, 4-5 ms 2039, 5-6 ms 289
(the set started before the sleep takes longer than 2.5 ms at times, so the end slides to 4-5 ms: a bimodal
end time, which itself straddles a pick-up point). Launch 2's ends were tight: 6-7 ms 6348 of 6493. Session
16's launch 3 (4000 us, clean): 4-5 ms 7083 of 7324. Race pacing fine in both (119.8 sets/s, 0.8 % / 0.3 %
late); compositor "mispresented" 121 / 332 (session 16 launch 3: 88).

Reading: ends at 6.5 ms are bad for the right eye and nearly clean for the left with a 2 s beat (the left
eye's edge drifts slowly against our wake-up clock); ends alternating 2.7 / 4.5 ms are bad for the right
eye. Ends at 4-5 ms are clean. The right eye's clean band is narrow and sits around 4-5 ms after the
wake-up. Session 18 maps it at 0.5 ms steps (4000 again, 4500, 3500) with the end distribution per launch.
