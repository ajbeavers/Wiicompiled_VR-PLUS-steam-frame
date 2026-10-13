// SPDX-License-Identifier: GPL-3.0-or-later
#include <aurora/vulkan_win32_interop.h>
#include "../internal.hpp"
#include "../stereo.hpp"
#include "../stereo_overlay.hpp"
#include "gpu.hpp"
#if (defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))) && defined(WEBGPU_DAWN) && defined(DAWN_ENABLE_BACKEND_VULKAN)
#if defined(_WIN32)
#include <windows.h>
#else
// The patched Dawn is linked statically on Linux; its Aurora Vulkan ABI is resolved at link time.
extern "C" {
uint32_t AuroraDawnVulkanVersion();
int AuroraDawnVulkanConfigure(const AuroraDawnVulkanHooks* hooks);
int AuroraDawnVulkanGetHandles(void* handle, AuroraDawnVulkanHandles* out);
void* AuroraDawnVulkanWrap(void* handle, const void* desc, uint64_t image);
int AuroraDawnVulkanRelease(void* handle, void* const* textures, uint32_t count);
void* AuroraDawnVulkanLock(void* handle);
void AuroraDawnVulkanUnlock(void* guard);
int AuroraDawnVulkanDrain(void* handle);
}
#endif
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
namespace aurora::vulkan_win32 {
namespace {
Module Log("aurora::vulkan_win32");
// KARTFRAME_FRAME_STAMP=1 (tests only): each eye image gets a 256x12 pixel block in its top left
// corner naming the display time its poses were located for and the eye, written after the eye
// copy. The mock runtime (MOCK_XR_STAMP) reads it back from the image a projection layer submits
// and compares it with the locate the layer's poses came from. Layout, 4x4 pixel cells, white = 1:
// row 0 the 64 bits of the display time (steady clock ns, bit 0 first), row 1 their complement,
// row 2: cell 0 the eye (1 = right), cell 1 its complement, cells 2-5 the marker 1 0 1 0.
constexpr uint32_t kStampCell = 4;
bool StampEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("KARTFRAME_FRAME_STAMP");
    return env != nullptr && env[0] == '1';
  }();
  return enabled;
}
// A 2x1 cell texture: black, then white.
wgpu::Texture StampPalette(wgpu::TextureFormat format) {
  static wgpu::Texture palette;
  static wgpu::TextureFormat paletteFormat = wgpu::TextureFormat::Undefined;
  if (palette && paletteFormat == format) return palette;
  const wgpu::Extent3D size{2 * kStampCell, kStampCell, 1};
  const wgpu::TextureDescriptor descriptor{.label = "Frame stamp palette",
                                           .usage = wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst,
                                           .dimension = wgpu::TextureDimension::e2D,
                                           .size = size,
                                           .format = format,
                                           .mipLevelCount = 1,
                                           .sampleCount = 1};
  palette = webgpu::g_device.CreateTexture(&descriptor);
  paletteFormat = format;
  std::array<uint8_t, 2 * kStampCell * kStampCell * 4> pixels{};
  for (uint32_t y = 0; y < kStampCell; ++y)
    for (uint32_t x = 0; x < 2 * kStampCell; ++x)
      for (uint32_t c = 0; c < 4; ++c) pixels[(y * 2 * kStampCell + x) * 4 + c] = x >= kStampCell || c == 3 ? 255 : 0;
  const wgpu::TexelCopyTextureInfo destination{.texture = palette};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = 2 * kStampCell * 4, .rowsPerImage = kStampCell};
  webgpu::g_queue.WriteTexture(&destination, pixels.data(), pixels.size(), &layout, &size);
  return palette;
}
void WriteStamp(wgpu::CommandEncoder& encoder, const wgpu::Texture& target, wgpu::TextureFormat format, uint64_t value,
                uint32_t eye) {
  const auto palette = StampPalette(format);
  const auto cell = [&](uint32_t column, uint32_t row, bool one) {
    wgpu::TexelCopyTextureInfo source, destination;
    source.texture = palette;
    source.origin = {one ? kStampCell : 0u, 0, 0};
    destination.texture = target;
    destination.origin = {column * kStampCell, row * kStampCell, 0};
    const wgpu::Extent3D size{kStampCell, kStampCell, 1};
    encoder.CopyTextureToTexture(&source, &destination, &size);
  };
  for (uint32_t bit = 0; bit < 64; ++bit) {
    const bool one = ((value >> bit) & 1u) != 0;
    cell(bit, 0, one);
    cell(bit, 1, !one);
  }
  cell(0, 2, eye == 1);
  cell(1, 2, eye != 1);
  for (uint32_t k = 0; k < 4; ++k) cell(2 + k, 2, (k & 1u) == 0);
}
// Set on the frame worker around the submitted callback (see aurora_vulkan_win32_submitting_retained).
thread_local bool submittingRetained = false;
struct Api {
  AuroraDawnVulkanConfigureFn configure = nullptr;
  AuroraDawnVulkanHandlesFn handles = nullptr;
  AuroraDawnVulkanWrapFn wrap = nullptr;
  AuroraDawnVulkanReleaseFn release = nullptr;
  AuroraDawnVulkanLockFn lock = nullptr;
  AuroraDawnVulkanUnlockFn unlock = nullptr;
  AuroraDawnVulkanDrainFn drain = nullptr;
  bool Load() {
#if defined(_WIN32)
    HMODULE module = GetModuleHandleW(L"webgpu_dawn.dll");
    if (!module) return false;
    auto version = reinterpret_cast<AuroraDawnVulkanVersionFn>(GetProcAddress(module, "AuroraDawnVulkanVersion"));
    if (!version || version() != AURORA_DAWN_VULKAN_ABI) return false;
#define LOAD(member, type, name) member = reinterpret_cast<type>(GetProcAddress(module, name)); if (!member) return false
    LOAD(configure, AuroraDawnVulkanConfigureFn, "AuroraDawnVulkanConfigure");
    LOAD(handles, AuroraDawnVulkanHandlesFn, "AuroraDawnVulkanGetHandles");
    LOAD(wrap, AuroraDawnVulkanWrapFn, "AuroraDawnVulkanWrap");
    LOAD(release, AuroraDawnVulkanReleaseFn, "AuroraDawnVulkanRelease");
    LOAD(lock, AuroraDawnVulkanLockFn, "AuroraDawnVulkanLock");
    LOAD(unlock, AuroraDawnVulkanUnlockFn, "AuroraDawnVulkanUnlock");
    LOAD(drain, AuroraDawnVulkanDrainFn, "AuroraDawnVulkanDrain");
#undef LOAD
    return true;
#else
    if (AuroraDawnVulkanVersion() != AURORA_DAWN_VULKAN_ABI) return false;
    configure = &AuroraDawnVulkanConfigure;
    handles = &AuroraDawnVulkanGetHandles;
    wrap = &AuroraDawnVulkanWrap;
    release = &AuroraDawnVulkanRelease;
    lock = &AuroraDawnVulkanLock;
    unlock = &AuroraDawnVulkanUnlock;
    drain = &AuroraDawnVulkanDrain;
    return true;
#endif
  }
} api;
int64_t VkFormat(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm: return 37;
  case wgpu::TextureFormat::RGBA8UnormSrgb: return 43;
  case wgpu::TextureFormat::BGRA8Unorm: return 44;
  case wgpu::TextureFormat::BGRA8UnormSrgb: return 50;
  case wgpu::TextureFormat::RGBA16Float: return 97;
  default: return 0;
  }
}
bool CopyCompatible(int64_t a, int64_t b) {
  return a == b || ((a == 37 || a == 43) && (b == 37 || b == 43)) ||
                  ((a == 44 || a == 50) && (b == 44 || b == 50));
}
struct Import { uint64_t image; uint32_t width, height; wgpu::TextureFormat format; wgpu::Texture texture; };
// The eyes, then the settings panel's layer image after them.
constexpr uint32_t kMaxImages = 3;
class Bridge {
public:
  std::mutex mutex;
  std::vector<Import> imports;
  std::array<AuroraD3D12StereoTarget, kMaxImages> targets{};
  std::array<wgpu::Texture, kMaxImages> active{};
  uint64_t token = 0;
  uint32_t count = 0;
  // Images this frame copies: `count` eyes, plus the panel when `panel` is set.
  uint32_t images = 0;
  bool panel = false;
  bool encoded = false;
  AuroraD3D12StereoSubmittedCallback callback;
  void* userdata;
  Bridge(AuroraD3D12StereoSubmittedCallback cb, void* data) : callback(cb), userdata(data) {}
  bool Set(uint64_t next, const AuroraD3D12StereoTarget* data, uint32_t n, const AuroraD3D12StereoTarget* panelTarget) {
    if (!next || !data || !n || n > 2) return false;
    std::lock_guard guard(mutex);
    if (token) return false;
    const auto valid = [](const AuroraD3D12StereoTarget& target) {
      return target.resource && target.width && target.height;
    };
    for (uint32_t i = 0; i < n; ++i) {
      if (!valid(data[i])) return false;
    }
    if (panelTarget && !valid(*panelTarget)) return false;
    for (uint32_t i = 0; i < n; ++i) targets[i] = data[i];
    panel = panelTarget != nullptr;
    if (panel) targets[n] = *panelTarget;
    token = next; count = n; images = n + (panel ? 1u : 0u); return true;
  }
  bool Cancel(uint64_t wanted) {
    std::unique_lock guard(mutex, std::try_to_lock);
    if (!guard.owns_lock() || encoded || !token || token != wanted) return false;
    token = 0; return true;
  }
  bool Encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) {
    std::lock_guard guard(mutex);
    if (!token || encoded || frame.frameToken != token) return false;
    std::array<stereo::EyeImage, kMaxImages> sources{};
    for (uint32_t i = 0; i < count; ++i) sources[i] = frame.eyes[i];
    if (panel && !stereo_overlay::layer_source(encoder, targets[count].width, targets[count].height, sources[count]))
      return false;
    // Validate/import every target before recording any copy.
    for (uint32_t i = 0; i < images; ++i) {
      const auto& eye = sources[i];
      const auto& target = targets[i];
      if (!eye.texture || eye.size.width != target.width || eye.size.height != target.height ||
          !CopyCompatible(VkFormat(eye.format), target.dxgiFormat)) return false;
      const uint64_t image = reinterpret_cast<uintptr_t>(target.resource);
      auto it = std::find_if(imports.begin(), imports.end(), [&](const Import& entry) { return entry.image == image; });
      if (it == imports.end()) {
        wgpu::TextureDescriptor descriptor;
        descriptor.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment;
        descriptor.size = {target.width, target.height, 1};
        descriptor.format = eye.format;
        void* wrapped = api.wrap(webgpu::g_device.Get(), &descriptor, image);
        if (!wrapped) return false;
        imports.push_back({image, target.width, target.height, eye.format,
                          wgpu::Texture::Acquire(static_cast<WGPUTexture>(wrapped))});
        it = imports.end() - 1;
      }
      if (it->width != target.width || it->height != target.height || it->format != eye.format) {
        // The runtime reuses VkImage handles across swapchain recreation, and
        // Aurora's eye format can change with the surface. Re-wrap rather than
        // rejecting every future frame for this image.
        imports.erase(it);
        --i;
        continue;
      }
      active[i] = it->texture;
    }
    for (uint32_t i = 0; i < images; ++i) {
      wgpu::TexelCopyTextureInfo source, destination;
      source.texture = *sources[i].texture;
      destination.texture = active[i];
      wgpu::Extent3D size{targets[i].width, targets[i].height, 1};
      encoder.CopyTextureToTexture(&source, &destination, &size);
      if (StampEnabled() && i < count && count == 2 && targets[i].width >= 64 * kStampCell &&
          targets[i].height >= 3 * kStampCell)
        WriteStamp(encoder, active[i], sources[i].format, frame.displayTimeNanos, i);
    }
    encoded = true;
    return true;
  }
  void Submitted(const stereo::SinkFrame& frame) {
    std::lock_guard guard(mutex);
    if (!token || !encoded || token != frame.frameToken) return;
    std::array<void*, kMaxImages> textures{};
    for (uint32_t i = 0; i < images; ++i) textures[i] = active[i].Get();
    // Append the COLOR_ATTACHMENT_OPTIMAL release barriers to Dawn's queue,
    // flush them under its device guard, then allow the XR thread to release.
    const bool success = api.release(webgpu::g_device.Get(), textures.data(), images) != 0;
    const auto completed = token;
    token = 0; encoded = false;
    submittingRetained = frame.retained;
    callback(completed, success, userdata);
    submittingRetained = false;
  }
};
std::unique_ptr<Bridge> bridge;
}
}
bool aurora_vulkan_win32_configure(const AuroraDawnVulkanHooks* hooks) {
  using namespace aurora::vulkan_win32;
  return api.Load() && api.configure(hooks);
}
bool aurora_vulkan_win32_get_handles(AuroraDawnVulkanHandles* handles, int64_t* format) {
  using namespace aurora;
  if (!webgpu::g_device || webgpu::g_backendType != wgpu::BackendType::Vulkan || !vulkan_win32::api.Load()) return false;
  *format = vulkan_win32::VkFormat(webgpu::g_graphicsConfig.surfaceConfiguration.format);
  return *format && vulkan_win32::api.handles(webgpu::g_device.Get(), handles);
}
bool aurora_vulkan_win32_enable(AuroraD3D12StereoSubmittedCallback cb, void* data) {
  using namespace aurora::vulkan_win32;
  if (bridge || !cb || !api.Load()) return false;
  bridge = std::make_unique<Bridge>(cb, data);
  aurora::stereo::set_sink(
      [](wgpu::CommandEncoder& encoder, const aurora::stereo::SinkFrame& frame, void* self) noexcept {
        return static_cast<Bridge*>(self)->Encode(encoder, frame);
      }, [](const aurora::stereo::SinkFrame& frame, void* self) noexcept { static_cast<Bridge*>(self)->Submitted(frame); }, bridge.get());
  return true;
}
bool aurora_vulkan_win32_set_targets(uint64_t token, const AuroraD3D12StereoTarget* targets, uint32_t count) {
  using namespace aurora::vulkan_win32;
  return bridge && bridge->Set(token, targets, count, nullptr);
}
bool aurora_vulkan_win32_set_targets_with_panel(uint64_t token, const AuroraD3D12StereoTarget* targets, uint32_t count,
                                                const AuroraD3D12StereoTarget* panel) {
  using namespace aurora::vulkan_win32;
  return bridge && bridge->Set(token, targets, count, panel);
}
bool aurora_vulkan_win32_cancel(uint64_t token) {
  using namespace aurora::vulkan_win32;
  return bridge && bridge->Cancel(token);
}
bool aurora_vulkan_win32_disable() {
  using namespace aurora::vulkan_win32;
  if (!bridge) return true;
  aurora::stereo::set_sink(nullptr, nullptr, nullptr);
  if (!api.drain(aurora::webgpu::g_device.Get())) { bridge.release(); return false; }
  bridge.reset();
  return true;
}
bool aurora_vulkan_win32_submitting_retained() { return aurora::vulkan_win32::submittingRetained; }
void* aurora_vulkan_win32_lock_queue() {
  return aurora::vulkan_win32::api.lock(aurora::webgpu::g_device.Get());
}
void aurora_vulkan_win32_unlock_queue(void* guard) { aurora::vulkan_win32::api.unlock(guard); }
#else
// C ABI stubs keep the runtime's OpenXR integration linkable on Windows GX
// builds whose Dawn has no Vulkan backend; the backend then reports that the
// bridge is unavailable and the game falls back to the desktop renderer.
bool aurora_vulkan_win32_configure(const AuroraDawnVulkanHooks*) { return false; }
bool aurora_vulkan_win32_get_handles(AuroraDawnVulkanHandles* handles, int64_t* format) {
  if (handles) *handles = {};
  if (format) *format = 0;
  return false;
}
bool aurora_vulkan_win32_enable(AuroraD3D12StereoSubmittedCallback, void*) { return false; }
bool aurora_vulkan_win32_set_targets(uint64_t, const AuroraD3D12StereoTarget*, uint32_t) { return false; }
bool aurora_vulkan_win32_set_targets_with_panel(uint64_t, const AuroraD3D12StereoTarget*, uint32_t,
                                                const AuroraD3D12StereoTarget*) { return false; }
bool aurora_vulkan_win32_cancel(uint64_t) { return false; }
bool aurora_vulkan_win32_disable() { return true; }
bool aurora_vulkan_win32_submitting_retained() { return false; }
void* aurora_vulkan_win32_lock_queue() { return nullptr; }
void aurora_vulkan_win32_unlock_queue(void*) {}
#endif
