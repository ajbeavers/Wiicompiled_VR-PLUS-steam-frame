// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR) && (defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__)))

#include "vr/openxr_backend.h"
#include "vr/openxr_runtime.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace mkw::vr {

// Shared frame vocabulary for the Windows pacing and replay paths.
using OpenXRWindowsVulkanFrameMode = OpenXRFrameMode;
using OpenXRWindowsVulkanBeginStatus = OpenXRBeginStatus;
using OpenXRWindowsVulkanSubmissionStatus = OpenXRSubmissionStatus;
using OpenXRWindowsVulkanPresentation = OpenXRPresentation;
using OpenXRWindowsVulkanFrame = OpenXRBackendFrame;

struct OpenXRWindowsVulkanGraphicsRequirements {
    XrVersion min_api_version = 0;
    XrVersion max_api_version = 0;
};

// Same-device Dawn/OpenXR Vulkan backend.
//
// Startup is deliberately split in two. QueryGraphicsRequirements() runs
// after OpenXRRuntime::Initialize() but before aurora_initialize(), allowing
// OpenXR to create Dawn's Vulkan instance/device and select the physical GPU.
// BindAurora() runs afterwards and checks the device before creating a session.
//
// All methods from BeginFrame() through FinishFrame(), plus PollEvents on the
// associated OpenXRRuntime, belong to one XR pacing thread. Aurora's frame
// worker never calls OpenXR: its post-submit callback only publishes a token
// that WaitForSubmission() consumes. This is the synchronization boundary
// required by the asynchronous sealed-frame renderer.
class OpenXRWindowsVulkanBackend final {
public:
    explicit OpenXRWindowsVulkanBackend(OpenXRLogCallback logger = {});
    ~OpenXRWindowsVulkanBackend();

    OpenXRWindowsVulkanBackend(const OpenXRWindowsVulkanBackend&) = delete;
    OpenXRWindowsVulkanBackend& operator=(const OpenXRWindowsVulkanBackend&) = delete;
    OpenXRWindowsVulkanBackend(OpenXRWindowsVulkanBackend&&) = delete;
    OpenXRWindowsVulkanBackend& operator=(OpenXRWindowsVulkanBackend&&) = delete;

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime);
    bool BindAurora(OpenXRRuntime& runtime);

    OpenXRWindowsVulkanBeginStatus BeginFrame(const OpenXRWindowsVulkanPresentation& presentation,
                                      OpenXRWindowsVulkanFrame& frame);

    // timeout_ms == UINT32_MAX waits until Aurora publishes this token or
    // Shutdown() interrupts the wait. A timeout does not release XR images;
    // the caller may keep pacing with RepeatFrame while Aurora still owns them.
    OpenXRWindowsVulkanSubmissionStatus WaitForSubmission(const OpenXRWindowsVulkanFrame& frame,
                                                  uint32_t timeout_ms = UINT32_MAX);

    // Withdraws this token only if Aurora has not encoded it. On success no GPU
    // command can reference the acquired images, and FinishFrame(frame, false)
    // is required to release them and close the compositor frame.
    bool TryCancelPendingFrame(OpenXRWindowsVulkanFrame& frame);

    // Ends the current compositor cycle with the last completed layer and starts
    // another, without releasing or changing Aurora's pending images/render token.
    // The original render poses remain attached to the pending and retained images.
    bool RepeatFrame(const OpenXRWindowsVulkanFrame& frame);

    // Releases acquired images and calls xrEndFrame. submit_layer must only be
    // true after WaitForSubmission returned Success. Immersive frames submit
    // XrCompositionLayerProjection; virtual-screen frames submit an
    // XrCompositionLayerQuad using the single mono target, placed as the
    // presentation's quad_anchored/quad_pose describe. If no new usable layer is
    // available, resubmits the retained layer using the current display time.
    bool FinishFrame(OpenXRWindowsVulkanFrame& frame, bool submit_layer);

    // Render-first path when interpolation is off. Aurora renders into acquired
    // non-retained XR images while no compositor frame is open. BeginFrameForPacket
    // accepts only a completed packet; CopyRenderedEyes verifies the already queued
    // bridge copy. FinishFrame releases the images and submits their original poses.
    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet);
    bool TryCancelPendingPacket(OpenXRBackendFrame& packet);
    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame);
    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame);
    OpenXRBeginStatus KeepAliveCycle();

    // Call on the XR owner thread after Aurora's worker is idle and before
    // aurora_shutdown(). Safe to repeat. False means a submitted Vulkan command
    // could not be fenced; the caller must retain this backend and its runtime
    // for the process lifetime instead of destroying possibly live resources.
    bool Shutdown();

    bool IsBound() const;
    // False once the settings panel's own layer could not be set up; the panel
    // is then drawn into the eyes again.
    bool PanelLayerAvailable() const;
    const OpenXRWindowsVulkanGraphicsRequirements& GraphicsRequirements() const;
    int64_t SwapchainFormat() const;
    const std::string& LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
