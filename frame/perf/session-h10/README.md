# Headset session 10 (lane O): KartFrame's hand-over to SteamVR made GalaxyFrame's

GalaxyFrame was clean on this headset; KartFrame doubles on head turns. frame/perf/H9.md lists every
difference in what the two apps hand SteamVR. This build makes the eye swapchains GalaxyFrame's by default
(R8G8B8A8_SRGB instead of B8G8R8A8_SRGB, COLOR_ATTACHMENT|SAMPLED instead of |TRANSFER_DST, a locate for the
frame's own time right before xrEndFrame); launch 1 also asks for GalaxyFrame's OpenXR extension list.

Install `01-galaxy-way.env` into `$KARTFRAME_HOME/session.queue/`. Add `02-format-alone.env` only after
launch 1 was clean (it then tells whether the format alone does it); if launch 1 still doubles, do not run
02 (report back instead).

**Routine, each launch:** menus, then straight into a Time Trial. During the countdown (kart standing
still) look at a track-side edge and turn the head slowly left and right for about ten seconds, hold still
five; then drive and do the same for ten seconds, hold still five. Say: doubled or not, which eye,
standing still and while driving; and whether the colours look normal (sky blue, not orange).

What the logs must show is in H9.md section 5.
