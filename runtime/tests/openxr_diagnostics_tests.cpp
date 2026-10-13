// SPDX-License-Identifier: GPL-3.0-or-later
// Headless checks for the F10 > Diagnostics OpenXR logging: the collector's
// window accounting, event classification and rate limit, driven with a
// synthetic clock, plus the global switch and its Config.toml key.

#include "vr/openxr_diagnostics.h"
#include "runtime_config.h"

#include <cstdlib>
#include <iostream>
#include <chrono>
#include <thread>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace mkw::vr::diagnostics;

void RequireAt(bool condition, int line, const char* expression) {
    if (!condition) {
        std::cerr << "openxr_diagnostics_tests.cpp:" << line << ": requirement failed: " << expression << '\n';
        std::abort();
    }
}
#define Require(condition) RequireAt((condition), __LINE__, #condition)

constexpr int64_t kMs = 1'000'000;
constexpr int64_t kPeriod72Hz = 13'888'889;

struct Capture {
    std::vector<std::string> lines;
    FrameDiagnostics::Sink Sink() {
        return [this](std::string_view line) { lines.emplace_back(line); };
    }
    size_t Count(std::string_view needle) const {
        size_t count = 0;
        for (const auto& line : lines) {
            count += line.find(needle) != std::string::npos ? 1 : 0;
        }
        return count;
    }
    const std::string& Summary() const {
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (it->find("cycles=") != std::string::npos) {
                return *it;
            }
        }
        static const std::string none;
        return none;
    }
};

// One compositor cycle on a 72 Hz grid: waits 1 ms, is begun, and ends
// `open_ns` later, `margin_ns` before its display deadline.
void Cycle(FrameDiagnostics& diagnostics, int64_t& now, int64_t& display_time, int64_t open_ns, int64_t margin_ns,
           int64_t slots = 1) {
    display_time += kPeriod72Hz * slots;
    now += kMs;
    const int64_t submit = now + open_ns;
    diagnostics.OnWaitFrame(now, kMs, display_time, kPeriod72Hz, submit + margin_ns);
    diagnostics.OnBeginFrame(now);
    diagnostics.OnEndFrame(submit, kMs / 2);
    now = submit + kMs / 2;
}

void TestSteadyWindow() {
    Capture capture;
    FrameDiagnostics diagnostics(capture.Sink());
    int64_t now = 5'000 * kMs;
    int64_t display_time = 1'000'000 * kMs;
    diagnostics.Reset(now);
    Require(diagnostics.ConsumeSessionInfoRequest());
    Require(!diagnostics.ConsumeSessionInfoRequest());
    const int64_t window_start = now;
    for (int frame = 0; frame < 72; ++frame) {
        diagnostics.OnFrameBegun(true, true, true, true, true);
        diagnostics.OnLayer(true);
        Cycle(diagnostics, now, display_time, 12 * kMs, 4 * kMs);
    }
    // 72 cycles of 13.5 ms fall just short of the window; nothing is emitted early.
    Require(capture.lines.empty());
    diagnostics.Tick(window_start + FrameDiagnostics::kWindowNs);
    const std::string& summary = capture.Summary();
    Require(!summary.empty());
    Require(summary.rfind("[xr-diag] ", 0) == 0);
    Require(summary.find("72.0Hz") != std::string::npos);
    Require(summary.find("skipped-slots=0 late=0") != std::string::npos);
    Require(summary.find("cycles=72 ") != std::string::npos);
    Require(summary.find("layers new=72 repeat=0 empty=0") != std::string::npos);
    Require(summary.find("end-margin=4.0/4.0") != std::string::npos);
    Require(summary.find("open=12.0/12.0") != std::string::npos);
    Require(summary.find("suppressed=0") != std::string::npos);
    // A steady run produces no event lines, only the summary.
    Require(capture.lines.size() == 1);
}

void TestLateSkippedAndStalledFrames() {
    Capture capture;
    FrameDiagnostics diagnostics(capture.Sink());
    int64_t now = 1'000 * kMs;
    int64_t display_time = 500'000 * kMs;
    diagnostics.Reset(now);
    Cycle(diagnostics, now, display_time, 5 * kMs, 3 * kMs);
    // Ends 2 ms after its display time, two slots after the previous frame.
    Cycle(diagnostics, now, display_time, 20 * kMs, -2 * kMs, 3);
    Require(capture.Count("late frame: xrEndFrame came 2.0 ms after") == 1);
    Require(capture.Count("compositor skipped 2 display slot(s)") == 1);
    // A 60 ms gap between xrEndFrame calls is a stall at 72 Hz.
    Cycle(diagnostics, now, display_time, 60 * kMs, 1 * kMs);
    Require(capture.Count("stall:") == 1);
    now += 2'000 * kMs;
    diagnostics.Tick(now);
    const std::string& summary = capture.Summary();
    Require(summary.find("cycles=3 skipped-slots=2 late=1") != std::string::npos);
}

void TestEventsAreRateLimited() {
    Capture capture;
    FrameDiagnostics diagnostics(capture.Sink());
    int64_t now = 1'000 * kMs;
    diagnostics.Reset(now);
    for (int frame = 0; frame < 20; ++frame) {
        diagnostics.OnEmptyFrame(EmptyFrameReason::NoRetainedLayer);
    }
    Require(capture.Count("empty frame submitted (no layers): no completed layer is retained") ==
            FrameDiagnostics::kMaxEventsPerWindow);
    now += 1'001 * kMs;
    diagnostics.Tick(now);
    Require(capture.Summary().find("empty=20") != std::string::npos);
    Require(capture.Summary().find("suppressed=12") != std::string::npos);
    // The next window has a fresh budget.
    diagnostics.OnRetainedLayerDiscarded(DiscardReason::ReferenceSpaceChanged);
    Require(capture.Count("retained layer discarded: the reference space changed") == 1);
}

void TestTrackingEdgesAndRejections() {
    Capture capture;
    FrameDiagnostics diagnostics(capture.Sink());
    diagnostics.Reset(1);
    diagnostics.OnFrameBegun(true, true, true, true, true);
    diagnostics.OnFrameBegun(true, true, true, true, false);
    diagnostics.OnFrameBegun(true, true, true, true, false);
    diagnostics.OnFrameBegun(true, true, true, true, true);
    // Views are not located when the runtime does not want rendering.
    diagnostics.OnFrameBegun(false, false, false, false, false);
    Require(capture.Count("tracking: head position lost") == 1);
    Require(capture.Count("tracking: head position regained") == 1);
    Require(capture.Count("orientation") == 0);

    Require(ClassifyRejectedLayer(false, false, false) == RejectReason::ReleaseFailed);
    Require(ClassifyRejectedLayer(true, false, true) == RejectReason::ShouldRenderOff);
    Require(ClassifyRejectedLayer(true, true, false) == RejectReason::ViewsInvalid);
    Require(ClassifyRejectedLayer(true, true, true) == RejectReason::PositionInvalid);
    diagnostics.OnLayerRejected(RejectReason::PositionInvalid);
    Require(capture.Count("rendered layer not submitted: the head position was not valid") == 1);

    diagnostics.Tick(2'000 * kMs);
    const std::string& summary = capture.Summary();
    Require(summary.find("immersive=4 screen=1 not-rendered=1") != std::string::npos);
    Require(summary.find("no-orientation=0 no-position=2") != std::string::npos);
    Require(summary.find("layer-rejected=1") != std::string::npos);
}

void TestPacketAccounting() {
    Capture capture;
    FrameDiagnostics diagnostics(capture.Sink());
    int64_t now = 10'000 * kMs;
    diagnostics.Reset(now);

    diagnostics.OnPacketPublished(now);
    diagnostics.OnSubmission(now + 18 * kMs, now + 15 * kMs, true);

    diagnostics.OnPacketPublished(now + 20 * kMs);
    diagnostics.OnPacketCanceled(now + 75 * kMs, 0);
    Require(capture.Count("no game frame picked it up before cancellation") == 1);

    diagnostics.OnPacketPublished(now + 80 * kMs);
    diagnostics.OnPacketCanceled(now + 280 * kMs, now + 90 * kMs);
    Require(capture.Count("Aurora picked it up but the bridge had not encoded it") == 1);

    diagnostics.OnPacketPublished(now + 400 * kMs);
    diagnostics.OnSubmission(now + 410 * kMs, now + 405 * kMs, false);
    Require(capture.Count("stereo submission failed") == 1);

    diagnostics.Tick(now + 1'500 * kMs);
    const std::string& summary = capture.Summary();
    Require(summary.find("packet-unused=1 packet-rejected=1 submit-failed=1") != std::string::npos);
    Require(capture.Count("cancel-age=200.0/200.0") == 1);
    Require(capture.Count("cancel-pickup=10.0/10.0") == 1);
    Require(capture.Count("cancel-after-pickup=190.0/190.0") == 1);
    // Two submissions: pickup 15 and 5 ms, render 3 and 5 ms.
    Require(summary.find("pickup=15.0/15.0") != std::string::npos);
    Require(summary.find("render=5.0/5.0") != std::string::npos);
}

void TestStageTimingSurvivesEventFlood() {
    Capture capture;
    FrameDiagnostics diagnostics(capture.Sink());
    const int64_t start = 10'000 * kMs;
    diagnostics.Reset(start);
    diagnostics.OnWaitFrame(start, 0, 1, kPeriod72Hz, 0);
    for (int i = 0; i < 20; ++i) diagnostics.OnEmptyFrame(EmptyFrameReason::NoRetainedLayer);
    diagnostics.OnStage(Stage::SyncActions, 2 * kMs, start + 2 * kMs);
    diagnostics.OnStage(Stage::SyncActions, 180 * kMs, start + 500 * kMs);
    diagnostics.OnStage(Stage::SyncActions, 3 * kMs, start + 600 * kMs);
    diagnostics.OnStage(Stage::Cancel, -1, start);
    diagnostics.Tick(start + 1'000 * kMs);
    Require(capture.Count("sync-actions=3.0/180.0@cycle=1,t=0.500s") == 1);
    Require(capture.Count("cancel=-") == 1);
    Require(capture.Summary().find("suppressed=12") != std::string::npos);
    diagnostics.OnStage(Stage::Cancel, 4 * kMs, start + 1'100 * kMs);
    diagnostics.Tick(start + 2'000 * kMs);
    Require(capture.Count("sync-actions=-") == 1);
    Require(capture.Count("cancel=4.0/4.0@cycle=1,t=1.100s") == 1);
    diagnostics.Reset(start + 3'000 * kMs);
    diagnostics.OnStage(Stage::Cancel, kMs, start + 3'010 * kMs);
    diagnostics.Tick(start + 4'000 * kMs);
    Require(capture.Count("cancel=1.0/1.0@cycle=0,t=0.010s") == 1);
}

void TestViewGeometry() {
    Capture capture;
    FrameDiagnostics diagnostics(capture.Sink());
    diagnostics.Reset(1);
    ViewGeometry pimax{};
    pimax.fov_degrees = {{{-60.0f, 45.0f, 50.0f, -50.0f}, {-45.0f, 60.0f, 50.0f, -50.0f}}};
    pimax.cant_degrees = 20.0f;
    pimax.ipd_millimeters = 64.0f;
    pimax.width = {4834, 4834};
    pimax.height = {4056, 4056};
    diagnostics.OnViewGeometry(pimax);
    Require(capture.Count("view geometry: swapchains 4834x4056 / 4834x4056") == 1);
    Require(capture.Count("eye cant 20.0 deg (canted displays)") == 1);
    Require(capture.Count("ipd 64.0 mm") == 1);
    // Tracking noise below the tolerance is not re-logged.
    pimax.ipd_millimeters = 64.2f;
    pimax.cant_degrees = 20.1f;
    diagnostics.OnViewGeometry(pimax);
    Require(capture.Count("view geometry") == 1);
    // Switching the headset to parallel projections is.
    pimax.cant_degrees = 0.0f;
    diagnostics.OnViewGeometry(pimax);
    Require(capture.Count("view geometry") == 2);
    Require(capture.Count("(parallel)") == 1);
    // Geometry and session descriptions are never rate-limited away.
    for (int event = 0; event < 20; ++event) {
        diagnostics.OnEmptyFrame(EmptyFrameReason::ShouldRenderOff);
    }
    diagnostics.OnReferenceSpaceChange("LOCAL", true, true, 12.5);
    Require(capture.Count("reference space change pending: LOCAL, previous pose valid, takes effect in 12.5 ms") == 1);
    diagnostics.OnReferenceSpaceChange("STAGE", false, false, 0.0);
    Require(capture.Count("reference space change pending: STAGE, previous pose not valid") == 1);
}

void TestResetStartsANewSession() {
    Capture capture;
    FrameDiagnostics diagnostics(capture.Sink());
    int64_t now = 1'000 * kMs;
    int64_t display_time = 10'000 * kMs;
    diagnostics.Reset(now);
    Require(diagnostics.ConsumeSessionInfoRequest());
    Cycle(diagnostics, now, display_time, 5 * kMs, 1 * kMs);
    diagnostics.Reset(now);
    Require(diagnostics.ConsumeSessionInfoRequest());
    // A new session's display clock must not read as thousands of skipped slots.
    display_time = 5 * kMs;
    Cycle(diagnostics, now, display_time, 5 * kMs, 1 * kMs);
    Require(capture.Count("skipped") == 0);
    Require(capture.Count("stall") == 0);
}

void TestGlobalSwitchAndConfig() {
    std::vector<std::string> lines;
    SetLogSink([&](std::string_view line) { lines.emplace_back(line); });
    Require(!Enabled());
    {
        const Stopwatch stopwatch;
        Require(stopwatch.ElapsedNs() == -1);
    }
    int calls = 0;
    Require(Measure(Stage::Cancel, [&] { ++calls; return 42; }) == 42);
    Require(calls == 1);
    OnEmptyFrame(EmptyFrameReason::NoRetainedLayer);
    Require(lines.empty());

    SetEnabled(true);
    Require(Enabled());
    Require(lines.size() == 1 && lines[0] == "[xr-diag] logging enabled");
    {
        const Stopwatch stopwatch;
        Require(stopwatch.ElapsedNs() >= 0);
    }
    Require(ConsumeSessionInfoRequest());
    Require(!ConsumeSessionInfoRequest());
    SetEnabled(true);
    Require(lines.size() == 1);
    SetEnabled(false);
    Require(lines.back() == "[xr-diag] logging disabled");
    Require(!ConsumeSessionInfoRequest());
    SetLogSink({});

    std::istringstream missing("[vr]\nenabled = true\n");
    Require(!RuntimeConfigFile::ParseConfig(missing).diagnosticsOpenXRLogging.has_value());
    std::istringstream on("[diagnostics]\nopenxr_logging = true\n");
    Require(RuntimeConfigFile::ParseConfig(on).diagnosticsOpenXRLogging == true);
    std::istringstream off("[diagnostics]\nopenxr_logging = false\n");
    Require(RuntimeConfigFile::ParseConfig(off).diagnosticsOpenXRLogging == false);
}

// The always-on pacing line: fixed format (frame/tools/session-report.py parses it).
void TestPacingLineFormat() {
    using namespace mkw::vr;
    std::vector<std::string> lines;
    pacing::SetSink([&](std::string_view line) { lines.emplace_back(line); });
    pacing::Reset();
    // Reset announces the session start clock ("vr pacing clock: ..."); the per-second line is
    // what this test checks.
    lines.clear();
    pacing::SetMode("pipelined");
    for (int i = 0; i < 120; ++i) {
        pacing::OnTick(120.0f, i == 7 ? 2u : 0u);
        pacing::OnRendered();
        pacing::NoteSetConsumed();
        pacing::OnSetSubmitted(diagnostics::detail::NowNs() + 3'000'000, i % 2 == 0);
        pacing::OnSetGpu(2.5 + (i == 50 ? 10.0 : 0.0));
    }
    pacing::OnLate();
    pacing::OnRepeat();
    pacing::Totals();  // totals only count flushed seconds: nothing yet
    Require(lines.empty());
    pacing::Flush();  // less than a second since Reset: no line
    Require(lines.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    pacing::Flush();
    Require(lines.size() == 1);
    const std::string& line = lines[0];
    Require(line.rfind("vr pacing: t=1 hz=120 mode=pipelined ticks=120 rendered=120 interpolated=60 late=1 missed=2 repeats=1 ", 0) == 0);
    Require(line.find(" gpu_ms=2.50/2.50 encode_ms=3.00/3.00") != std::string::npos);
    pacing::Totals();
    Require(lines.size() == 2 && lines[1].rfind("vr pacing totals: t=1 ", 0) == 0);
    pacing::Totals();  // printed once per session
    Require(lines.size() == 2);
    pacing::SetSink({});
}

} // namespace

int main() {
    TestPacingLineFormat();
    TestSteadyWindow();
    TestLateSkippedAndStalledFrames();
    TestEventsAreRateLimited();
    TestTrackingEdgesAndRejections();
    TestPacketAccounting();
    TestStageTimingSurvivesEventFlood();
    TestViewGeometry();
    TestResetStartsANewSession();
    TestGlobalSwitchAndConfig();
    std::cout << "OpenXR diagnostics tests passed\n";
}
