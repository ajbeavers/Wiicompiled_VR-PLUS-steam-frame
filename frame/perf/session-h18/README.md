# Headset session 18: the end-delay band at 0.5 ms steps (4.0 again, 4.5, 3.5)

Same build as sessions 16 and 17. Owner: "all of them had doubling in the right eye (launch 3, 3.5 ms,
maybe the least but still very present) and super slight in the left, especially in the water and visible
on the HUD."

Logs: ends tight in every launch (4000: 4-4.5 ms 4635 of 5743, 4.5-5 ms 838; 4500: 4.5-5 ms 5100 of 6335;
3500: 3.5-4 ms 7719 of 11951, 4-4.5 ms 2841). Session 16's clean 4000 launch had the same distribution
(4-4.5 ms 5821 of 7324, 4.5-5 ms 1262) and the same locate-to-display offset (43.7-43.8 ms p50 in all).
Nothing the app logs separates the clean 4000 launch from the doubled one.

Reading: the clean band is not a fixed offset from the app's wake-up; the relation between the app's frame
clock (xrWaitFrame) and the point in the refresh where the compositor takes each eye's image differs per
launch and drifts (session 17's 2 s beat). Ending later than the wake-up is the right lever (session 16),
but the end must be timed against the compositor's own clock, which the app cannot see today. Next: lane W
makes that phase observable (per-frame compositor timing beside the app's wake-up and end times) and
designs the lock.
