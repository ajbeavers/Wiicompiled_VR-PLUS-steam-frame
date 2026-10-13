#pragma once

#include "webgpu/gpu.hpp"

#include <array>
#include <cstdint>

namespace aurora::stereo {

struct EyeImage {
  // Borrowed for the duration of the sink callback. A sink may encode work
  // that reads the texture, but must not retain these pointers.
  const wgpu::Texture* texture = nullptr;
  const wgpu::TextureView* view = nullptr;
  wgpu::Extent3D size{};
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
};

struct SinkFrame {
  uint64_t frameToken = 0;
  uint32_t logicalFrame = 0;
  AuroraStereoFrameMode mode = AURORA_STEREO_FRAME_IMMERSIVE_REPLAY;
  // Rendered from the retained game frame (interpolation, projected menu), not a new one.
  bool retained = false;
  std::array<EyeImage, AURORA_STEREO_EYE_COUNT> eyes{};
  // The packet's display time (steady clock ns): the time its eye poses were located for.
  uint64_t displayTimeNanos = 0;
};

// Runs synchronously on the frame worker after both eye replays have been
// encoded and before its command buffer is submitted. Backend interop code
// can append copies/import transitions to the same encoder here.
using SinkCallback = bool (*)(wgpu::CommandEncoder& encoder, const SinkFrame& frame, void* userdata) noexcept;
// Called immediately after the command buffer containing the sink's encoded
// work is submitted to Dawn's queue, while Aurora's queue-submit mutex remains
// held. It may enqueue native work on that same queue, but must not wait or
// call back into Aurora. This is the handoff point for backend-native follow-up
// work; its completion notification may wake the owner thread to release the
// acquired OpenXR images and end the frame.
using SubmitCallback = void (*)(const SinkFrame& frame, void* userdata) noexcept;

// Internal Aurora hook: D3D12/Vulkan OpenXR interop owns this registration.
// Registration changes must happen while the frame worker is idle.
void set_sink(SinkCallback callback, SubmitCallback submitted, void* userdata) noexcept;
inline void set_sink(SinkCallback callback, void* userdata) noexcept { set_sink(callback, nullptr, userdata); }

// Optional, for a sink that can take the eyes as render targets (the Vulkan OpenXR bridge): the
// swapchain images acquired for `frameToken`, wrapped as textures of `format` and `width` x `height`,
// so the eyes render straight into them and the sink only stamps them instead of copying 2 x 12 MB a
// set (lane I: 1.3 ms of GPU a set at 1728). False when the set cannot take them (no pending set
// for that token, other sizes or formats); the eyes then render into Aurora's own targets and the sink
// copies as before. Once it returned true the set can no longer be canceled: the caller must hand
// the same textures to the sink (SinkFrame::eyes) for this token.
using DirectTargetsCallback = bool (*)(uint64_t frameToken, wgpu::TextureFormat format, uint32_t width,
                                       uint32_t height, std::array<wgpu::Texture, AURORA_STEREO_EYE_COUNT>& out,
                                       void* userdata) noexcept;
void set_direct_targets(DirectTargetsCallback callback, void* userdata) noexcept;

} // namespace aurora::stereo
