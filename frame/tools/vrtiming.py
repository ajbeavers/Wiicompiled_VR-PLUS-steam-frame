#!/usr/bin/env python3
# GalaxyFrame: SteamVR's compositor frame timing, a line a
# second, for whatever app has the scene (IVRCompositor::GetFrameTimings and
# GetCumulativeStats through ctypes).  Read-only: it connects as a background
# app and changes nothing.  The game's own log counts the frames it was late
# for (shown twice, missed refreshes); this shows what the compositor did with
# the frames it got (reused, mispresented, dropped, reprojected), and the
# compositor's GPU time.  tools/session-report.py reads both logs.
# Usage: env -u LD_LIBRARY_PATH python3 -I vrtiming.py SECONDS [out.txt]
# frame/kartframe.sh runs it for the length of a session into
# ~/.local/share/GalaxyFrame/vrtiming.log (GQ_VRTIMING=0 turns that off).
# Origin: tools/vrtiming.py of Moonlight XR Native (GPLv3).
#
# KartFrame lane W: KARTFRAME_VRTIMING_FRAMES=1 also writes EVERY compositor frame (de-duplicated by
# frame index; the newest two of each poll wait for the next poll, they may still be filling in) to
# vrframes.log beside out.txt (or KARTFRAME_VRFRAMES=path), plus a P line per poll that ties SteamVR's
# clock to CLOCK_MONOTONIC (the game's XRF m=/t= clock): CLOCK_MONOTONIC, _RAW and BOOTTIME read around
# IVRSystem::GetTimeSinceLastVsync, and the newest frame's m_flSystemTimeInSeconds. Format in the file's
# header; frame/tools/phase.py reads it. KARTFRAME_STEAMVR_DIR: where libopenvr_api.so is (tests).
import ctypes, os, signal, sys, time

class HmdMatrix34(ctypes.Structure):
    _fields_ = [('m', ctypes.c_float * 12)]
class HmdVector3(ctypes.Structure):
    _fields_ = [('v', ctypes.c_float * 3)]
class TrackedDevicePose(ctypes.Structure):
    _fields_ = [('mDeviceToAbsoluteTracking', HmdMatrix34), ('vVelocity', HmdVector3),
                ('vAngularVelocity', HmdVector3), ('eTrackingResult', ctypes.c_int),
                ('bPoseIsValid', ctypes.c_bool), ('bDeviceIsConnected', ctypes.c_bool)]
class FrameTiming(ctypes.Structure):
    _fields_ = [('m_nSize', ctypes.c_uint32), ('m_nFrameIndex', ctypes.c_uint32),
                ('m_nNumFramePresents', ctypes.c_uint32), ('m_nNumMisPresented', ctypes.c_uint32),
                ('m_nNumDroppedFrames', ctypes.c_uint32), ('m_nReprojectionFlags', ctypes.c_uint32),
                ('m_flSystemTimeInSeconds', ctypes.c_double)] + [(n, ctypes.c_float) for n in (
                'm_flPreSubmitGpuMs', 'm_flPostSubmitGpuMs', 'm_flTotalRenderGpuMs', 'm_flCompositorRenderGpuMs',
                'm_flCompositorRenderCpuMs', 'm_flCompositorIdleCpuMs', 'm_flClientFrameIntervalMs',
                'm_flPresentCallCpuMs', 'm_flWaitForPresentCpuMs', 'm_flSubmitFrameMs', 'm_flWaitGetPosesCalledMs',
                'm_flNewPosesReadyMs', 'm_flNewFrameReadyMs', 'm_flCompositorUpdateStartMs',
                'm_flCompositorUpdateEndMs', 'm_flCompositorRenderStartMs')] + [
                ('m_HmdPose', TrackedDevicePose), ('m_nNumVSyncsReadyForUse', ctypes.c_uint32),
                ('m_nNumVSyncsToFirstView', ctypes.c_uint32)]
class CumulativeStats(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in (
                'm_nPid', 'm_nNumFramePresents', 'm_nNumDroppedFrames', 'm_nNumReprojectedFrames',
                'm_nNumFramePresentsOnStartup', 'm_nNumDroppedFramesOnStartup', 'm_nNumReprojectedFramesOnStartup',
                'm_nNumLoading', 'm_nNumFramePresentsLoading', 'm_nNumDroppedFramesLoading',
                'm_nNumReprojectedFramesLoading', 'm_nNumTimedOut', 'm_nNumFramePresentsTimedOut',
                'm_nNumDroppedFramesTimedOut', 'm_nNumReprojectedFramesTimedOut', 'm_nNumFrameSubmits')] + [
                (n, ctypes.c_double) for n in ('m_flSumCompositorCPUTimeMS', 'm_flSumCompositorGPUTimeMS',
                'm_flSumTargetFrameTimes', 'm_flSumApplicationCPUTimeMS', 'm_flSumApplicationGPUTimeMS')] + [
                ('m_nNumFramesWithDepth', ctypes.c_uint32)]

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30
out = open(sys.argv[2], 'w') if len(sys.argv) > 2 else None
def say(s):
    print(s, flush=True)
    if out: out.write(s + '\n'); out.flush()

# The wrapper stops this with SIGTERM: leave the loop and flush the files.
stop = []
signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
lib = ctypes.CDLL(os.path.join(os.environ.get('KARTFRAME_STEAMVR_DIR') or '/opt/steamvr/bin/linuxarm64',
                               'libopenvr_api.so'))
err = ctypes.c_int(0)
lib.VR_InitInternal2.restype = ctypes.c_uint32
lib.VR_InitInternal2.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_char_p]
t_end = time.time() + seconds
# SteamVR may come up after the launcher: try again every 5 s until it does
while True:
    lib.VR_InitInternal2(ctypes.byref(err), 3, None)   # VRApplication_Background
    if not err.value:
        break
    if time.time() + 5 >= t_end or stop:
        say('VR init failed %d (is SteamVR running?)' % err.value); sys.exit(1)
    time.sleep(5)
lib.VR_GetGenericInterface.restype = ctypes.c_void_p
lib.VR_GetGenericInterface.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
p = lib.VR_GetGenericInterface(b'FnTable:IVRCompositor_027', ctypes.byref(err))
if not p:
    say('no IVRCompositor_027 (%d)' % err.value); sys.exit(1)
table = (ctypes.c_void_p * 12).from_address(p)
GetFrameTimings = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.POINTER(FrameTiming), ctypes.c_uint32)(table[9])
GetCumulativeStats = ctypes.CFUNCTYPE(None, ctypes.POINTER(CumulativeStats), ctypes.c_uint32)(table[11])

# Lane W: the per-frame log (KARTFRAME_VRTIMING_FRAMES=1).
frames_out = None
if os.environ.get('KARTFRAME_VRTIMING_FRAMES') == '1':
    path = os.environ.get('KARTFRAME_VRFRAMES') or os.path.join(
        os.path.dirname(os.path.abspath(sys.argv[2])) if out else '.', 'vrframes.log')
    frames_out = open(path, 'w', buffering=1 << 16)
    sysp = lib.VR_GetGenericInterface(b'FnTable:IVRSystem_022', ctypes.byref(err))
    # IVRSystem::GetTimeSinceLastVsync (slot 5): seconds since the last vsync and SteamVR's frame counter.
    SinceVsync = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_uint64))(
        (ctypes.c_void_p * 6).from_address(sysp)[5]) if sysp else None
    CM, CR, CB = time.CLOCK_MONOTONIC, getattr(time, 'CLOCK_MONOTONIC_RAW', time.CLOCK_MONOTONIC), getattr(
        time, 'CLOCK_BOOTTIME', time.CLOCK_MONOTONIC)
    frames_out.write(
        '# vrframes v1 %s (KartFrame frame/tools/vrtiming.py, KARTFRAME_VRTIMING_FRAMES=1)\n'
        '# F idx sys_s presents mispresented dropped reproj_flags | ms after sys_s: waitgetposes posesready '
        'frameready updatestart updateend renderstart | ms: present_cpu waitforpresent_cpu idle_cpu comp_gpu '
        'interval | vsyncs_ready vsyncs_to_first_view\n'
        '# P mono_s raw_s boot_s since_vsync_s vsync_counter call_us newest_idx newest_sys_s '
        '(clocks read around GetTimeSinceLastVsync; mono = CLOCK_MONOTONIC = the game XRF m=/t= clock)\n'
        '# G first_idx last_idx (frames missed between polls)\n' % time.strftime('%Y-%m-%d %H:%M:%S'))
frames_last = None
fv = ctypes.c_float(0.0)
fc = ctypes.c_uint64(0)

def log_frames(buf, n):
    global frames_last
    if n < 3: return
    newest = buf[n - 1]
    m0 = time.clock_gettime(CM)
    ok = SinceVsync(ctypes.byref(fv), ctypes.byref(fc)) if SinceVsync else False
    m1 = time.clock_gettime(CM)
    raw, boot = time.clock_gettime(CR), time.clock_gettime(CB)
    w = frames_out.write
    w('P %.6f %.6f %.6f %.6f %d %d %d %.6f\n' % (
        (m0 + m1) / 2, raw, boot, fv.value if ok else -1.0, fc.value if ok else 0, (m1 - m0) * 1e6,
        newest.m_nFrameIndex, newest.m_flSystemTimeInSeconds))
    for i in range(n - 2):          # the newest two wait for the next poll
        f = buf[i]
        k = f.m_nFrameIndex
        if frames_last is not None and k <= frames_last: continue
        if frames_last is not None and k > frames_last + 1: w('G %d %d\n' % (frames_last + 1, k - 1))
        frames_last = k
        w('F %d %.6f %d %d %d %d %.3f %.3f %.3f %.3f %.3f %.3f %.2f %.2f %.2f %.2f %.2f %d %d\n' % (
            k, f.m_flSystemTimeInSeconds, f.m_nNumFramePresents, f.m_nNumMisPresented, f.m_nNumDroppedFrames,
            f.m_nReprojectionFlags, f.m_flWaitGetPosesCalledMs, f.m_flNewPosesReadyMs, f.m_flNewFrameReadyMs,
            f.m_flCompositorUpdateStartMs, f.m_flCompositorUpdateEndMs, f.m_flCompositorRenderStartMs,
            f.m_flPresentCallCpuMs, f.m_flWaitForPresentCpuMs, f.m_flCompositorIdleCpuMs,
            f.m_flCompositorRenderGpuMs, f.m_flClientFrameIntervalMs, f.m_nNumVSyncsReadyForUse,
            f.m_nNumVSyncsToFirstView))

def cumulative():
    c = CumulativeStats()
    GetCumulativeStats(ctypes.byref(c), ctypes.sizeof(c))
    return c

N = 128
buf = (FrameTiming * N)()
c0 = cumulative()
say('# scene app pid %d; per second: compositor frames, app frames reused (presents>1), mispresented, dropped, '
    'reprojected cpu/gpu, async on, motion, compositor gpu ms, app interval ms avg/max, poses->frame ready ms avg/max, '
    'vsyncs to photons' % c0.m_nPid)
say('# %s' % time.strftime('%Y-%m-%d %H:%M:%S'))
say('time      frames reused mispres dropped rp_cpu rp_gpu async motion  compGPU  interval(avg/max)  app(avg/max)  vsyncs')
seen = set()
win = []
t_next = time.time() + 1.0
pid = c0.m_nPid
totals = dict(frames=0, reused=0, mis=0, dropped=0, cpu=0, gpu=0)
while time.time() < t_end and not stop:
    time.sleep(0.25)   # 128 frames are 1.07 s at 120 Hz: no frame is missed
    buf[0].m_nSize = ctypes.sizeof(FrameTiming)
    n = GetFrameTimings(buf, N)
    if frames_out: log_frames(buf, n)
    for i in range(n):
        f = buf[i]
        if f.m_nFrameIndex in seen: continue
        seen.add(f.m_nFrameIndex)
        win.append((f.m_nFrameIndex, f.m_nNumFramePresents, f.m_nNumMisPresented, f.m_nNumDroppedFrames,
                    f.m_nReprojectionFlags, f.m_flCompositorRenderGpuMs, f.m_flClientFrameIntervalMs,
                    f.m_flNewFrameReadyMs - f.m_flNewPosesReadyMs, f.m_nNumVSyncsToFirstView))
    if time.time() >= t_next and win:
        t_next += 1.0
        now_pid = cumulative().m_nPid
        if now_pid != pid:
            pid = now_pid
            say('# %s scene app now pid %d' % (time.strftime('%H:%M:%S'), pid))
        k = len(win)
        reused = sum(1 for w in win if w[1] > 1)
        mis = sum(w[2] for w in win); dropped = sum(w[3] for w in win)
        cpu = sum(1 for w in win if w[4] & 1); gpu = sum(1 for w in win if w[4] & 2)
        asy = sum(1 for w in win if w[4] & 4); mot = sum(1 for w in win if w[4] & 8)
        comp = sum(w[5] for w in win) / k
        ivs = [w[6] for w in win]; app = [w[7] for w in win if w[7] > 0]
        vs = sum(w[8] for w in win) / k
        say('%s %6d %6d %7d %7d %6d %6d %5d %6d  %6.2f   %6.2f/%-7.2f    %5.2f/%-6.2f  %5.2f' % (
            time.strftime('%H:%M:%S'), k, reused, mis, dropped, cpu, gpu, asy, mot, comp,
            sum(ivs) / k, max(ivs), (sum(app) / len(app)) if app else 0, max(app) if app else 0, vs))
        totals['frames'] += k; totals['reused'] += reused; totals['mis'] += mis
        totals['dropped'] += dropped; totals['cpu'] += cpu; totals['gpu'] += gpu
        win = []
    if len(seen) > 100000: seen = set(list(seen)[-1000:])
c1 = cumulative()
say('# window totals: %s' % totals)
say('# SteamVR cumulative for pid %d over the run: presents %d, dropped %d, reprojected %d, app submits %d, '
    'frames with depth %d' % (
    c1.m_nPid, c1.m_nNumFramePresents - (c0.m_nNumFramePresents if c0.m_nPid == c1.m_nPid else 0),
    c1.m_nNumDroppedFrames - (c0.m_nNumDroppedFrames if c0.m_nPid == c1.m_nPid else 0),
    c1.m_nNumReprojectedFrames - (c0.m_nNumReprojectedFrames if c0.m_nPid == c1.m_nPid else 0),
    c1.m_nNumFrameSubmits - (c0.m_nNumFrameSubmits if c0.m_nPid == c1.m_nPid else 0),
    c1.m_nNumFramesWithDepth - (c0.m_nNumFramesWithDepth if c0.m_nPid == c1.m_nPid else 0)))
if frames_out: frames_out.close()
lib.VR_ShutdownInternal()
