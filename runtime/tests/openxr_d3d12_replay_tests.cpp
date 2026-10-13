// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real backend against a deterministic compositor and Aurora sink.
// No headset, graphics driver, OpenXR loader, or translated game is required.
#if defined(TEST_WINDOWS_VULKAN)
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr_platform.h>
#include <aurora/vulkan_win32_interop.h>
#include "vr/openxr_vulkan_win32.h"
#define OpenXRD3D12Backend OpenXRWindowsVulkanBackend
#define OpenXRD3D12BeginStatus OpenXRWindowsVulkanBeginStatus
#define OpenXRD3D12SubmissionStatus OpenXRWindowsVulkanSubmissionStatus
#define OpenXRD3D12Frame OpenXRWindowsVulkanFrame
#define OpenXRD3D12Presentation OpenXRWindowsVulkanPresentation
#define OpenXRD3D12FrameMode OpenXRWindowsVulkanFrameMode
#define aurora_d3d12_enable_stereo_bridge aurora_vulkan_win32_enable
#define aurora_d3d12_set_stereo_targets aurora_vulkan_win32_set_targets
#define aurora_d3d12_set_stereo_targets_with_panel aurora_vulkan_win32_set_targets_with_panel
#define aurora_d3d12_cancel_stereo_targets aurora_vulkan_win32_cancel
#define aurora_d3d12_disable_stereo_bridge aurora_vulkan_win32_disable
#else
#define CINTERFACE
#define XR_USE_GRAPHICS_API_D3D12
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <openxr/openxr_platform.h>
#include <aurora/d3d12_interop.h>
#include "vr/openxr_d3d12.h"
#endif

#include "vr/openxr_wii_remote.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace mkw::vr;

namespace {
void RequireAt(bool condition, int line, const char* expression) {
    if (!condition) {
        std::cerr << "OpenXR replay test failed at line " << line << ": " << expression << '\n';
        std::abort();
    }
}
#define Require(condition) RequireAt((condition), __LINE__, #condition)
struct Image { uint64_t content = 0; };
struct Swapchain {
    Image image;
    bool acquired = false;
    bool waited = false;
    bool released = false;
};
std::vector<AuroraD3D12StereoTarget> targets;
AuroraD3D12StereoTarget panel_target{};
bool has_panel_target = false;
AuroraD3D12StereoSubmittedCallback callback = nullptr;
void* callback_data = nullptr;
uint64_t pending_token = 0;
bool encoded = false;
bool compositor_open = false;
bool expect_render_first = false;
bool should_render = true;
bool tracking_valid = true;
bool change_space = false;
bool restart_session = false;
bool stop_session = false;
bool fail_end = false;
uint64_t displayed_content = 0;
uint32_t layer_count = 0;
uint32_t releases = 0;
uint32_t live_swapchains = 0;
XrTime display_time = 0;
XrStructureType layer_type = XR_TYPE_UNKNOWN;
XrPosef quad_pose{};
uint64_t panel_content = 0; // what the settings panel's layer showed, 0 without one
XrPosef panel_pose{};
XrExtent2Df panel_size{};

void Complete(bool success = true) {
    Require(pending_token != 0);
    if (expect_render_first) Require(!compositor_open);
    if (success) {
        for (const auto& target : targets)
            reinterpret_cast<Image*>(target.resource)->content = pending_token;
        if (has_panel_target)
            reinterpret_cast<Image*>(panel_target.resource)->content = pending_token;
    }
    callback(pending_token, success, callback_data);
    pending_token = 0;
    encoded = false;
}

#if defined(TEST_WINDOWS_VULKAN)
AuroraDawnVulkanHooks hooks{};
bool queue_locked = false;
XrResult XRAPI_CALL Requirements(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR* out) {
    out->minApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
    out->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL Physical(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR*, VkPhysicalDevice* out) {
    *out = reinterpret_cast<VkPhysicalDevice>(2); return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateInstance(XrInstance, const XrVulkanInstanceCreateInfoKHR* info, VkInstance* out, VkResult* result) {
    Require(info->vulkanCreateInfo->pApplicationInfo->apiVersion >= VK_API_VERSION_1_1);
    *out = reinterpret_cast<VkInstance>(1); *result = VK_SUCCESS; return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateDevice(XrInstance, const XrVulkanDeviceCreateInfoKHR* info, VkDevice* out, VkResult* result) {
    Require(info->vulkanPhysicalDevice == reinterpret_cast<VkPhysicalDevice>(2));
    *out = reinterpret_cast<VkDevice>(3); *result = VK_SUCCESS; return XR_SUCCESS;
}
#else
HRESULT STDMETHODCALLTYPE FeatureSupport(ID3D12Device*, D3D12_FEATURE,
                                         void* data, UINT) {
    static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(data)->MaxSupportedFeatureLevel =
        D3D_FEATURE_LEVEL_11_0;
    return S_OK;
}
XrResult XRAPI_CALL Requirements(XrInstance, XrSystemId,
                               XrGraphicsRequirementsD3D12KHR* requirements) {
    requirements->adapterLuid = {};
    requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}
#endif
}

// A COM vtable in its C representation supplies the single device operation
// used by BindAurora. The production backend is compiled normally as C++.
#if defined(TEST_WINDOWS_VULKAN)
bool aurora_vulkan_win32_configure(const AuroraDawnVulkanHooks* value) {
    hooks = value ? *value : AuroraDawnVulkanHooks{}; return true;
}
bool aurora_vulkan_win32_get_handles(AuroraDawnVulkanHandles* handles, int64_t* format) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance.pApplicationInfo = &app;
    Require(hooks.createInstance(hooks.userdata, nullptr, &instance, nullptr, &handles->instance) == VK_SUCCESS);
    Require(hooks.getPhysicalDevice(hooks.userdata, handles->instance, &handles->physicalDevice) == VK_SUCCESS);
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    Require(hooks.createDevice(hooks.userdata, nullptr, handles->physicalDevice, &device, nullptr, &handles->device) == VK_SUCCESS);
    *format = VK_FORMAT_R8G8B8A8_UNORM; return true;
}
void* aurora_vulkan_win32_lock_queue() { Require(!queue_locked); queue_locked = true; return &queue_locked; }
void aurora_vulkan_win32_unlock_queue(void*) { Require(queue_locked); queue_locked = false; }
bool aurora_vulkan_win32_submitting_retained() { return false; }
#else
bool aurora_d3d12_get_native_handles(AuroraD3D12NativeHandles* handles) {
    static ID3D12DeviceVtbl vtable{};
    vtable.CheckFeatureSupport = FeatureSupport;
    static ID3D12Device device{&vtable};
    *handles = {&device, &device, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0};
    return true;
}
#endif
bool aurora_d3d12_enable_stereo_bridge(AuroraD3D12StereoSubmittedCallback cb, void* data) {
    callback = cb;
    callback_data = data;
    return true;
}
bool aurora_d3d12_set_stereo_targets_with_panel(uint64_t token, const AuroraD3D12StereoTarget* data,
                                                uint32_t count, const AuroraD3D12StereoTarget* panel) {
    Require(pending_token == 0);
    pending_token = token;
    targets.assign(data, data + count);
    has_panel_target = panel != nullptr;
    panel_target = panel ? *panel : AuroraD3D12StereoTarget{};
    return true;
}
bool aurora_d3d12_set_stereo_targets(uint64_t token, const AuroraD3D12StereoTarget* data,
                                   uint32_t count) {
    return aurora_d3d12_set_stereo_targets_with_panel(token, data, count, nullptr);
}
bool aurora_d3d12_cancel_stereo_targets(uint64_t token) {
    if (encoded || token != pending_token) return false;
    pending_token = 0;
    return true;
}
bool aurora_d3d12_disable_stereo_bridge() {
    pending_token = 0;
    encoded = false;
    return true; // Simulate a successful queue drain.
}

XrResult XRAPI_CALL xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain* out) {
    *out = reinterpret_cast<XrSwapchain>(new Swapchain);
    ++live_swapchains;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain handle, uint32_t capacity,
                                             uint32_t* count, XrSwapchainImageBaseHeader* images) {
    *count = 1; // Single-image swapchains also require a separate retained pair.
#if defined(TEST_WINDOWS_VULKAN)
    if (capacity) reinterpret_cast<XrSwapchainImageVulkanKHR*>(images)->image =
        reinterpret_cast<VkImage>(&reinterpret_cast<Swapchain*>(handle)->image);
#else
    if (capacity) reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)->texture =
        reinterpret_cast<ID3D12Resource*>(&reinterpret_cast<Swapchain*>(handle)->image);
#endif
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain handle,
                                          const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    auto& chain = *reinterpret_cast<Swapchain*>(handle);
#if defined(TEST_WINDOWS_VULKAN)
    Require(queue_locked);
#endif
    Require(!chain.acquired);
    chain.acquired = true;
    chain.waited = false;
    *index = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain handle, const XrSwapchainImageWaitInfo*) {
    auto& chain = *reinterpret_cast<Swapchain*>(handle);
    Require(chain.acquired);
    chain.waited = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain handle,
                                          const XrSwapchainImageReleaseInfo*) {
    auto& chain = *reinterpret_cast<Swapchain*>(handle);
#if defined(TEST_WINDOWS_VULKAN)
    Require(queue_locked); // The runtime may touch Dawn's VkQueue here.
#endif
    Require(chain.acquired && chain.waited);
    chain.acquired = false;
    chain.released = true;
    ++releases;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain handle) {
    auto* chain = reinterpret_cast<Swapchain*>(handle);
    Require(!chain->acquired);
    delete chain;
    --live_swapchains;
    return XR_SUCCESS;
}

namespace mkw::vr {
OpenXRRuntime::OpenXRRuntime(OpenXRLogCallback) {
    m_instance = reinterpret_cast<XrInstance>(this);
#if defined(TEST_WINDOWS_VULKAN)
    m_swapchain_formats = {VK_FORMAT_R8G8B8A8_SRGB};
#else
    m_swapchain_formats = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
#endif
    for (auto& view : m_view_configuration) {
        view.render_width = 100;
        view.render_height = 80;
    }
}
OpenXRRuntime::~OpenXRRuntime() = default;
bool OpenXRRuntime::GetInstanceProcAddress(const char* name, PFN_xrVoidFunction* out) {
#if defined(TEST_WINDOWS_VULKAN)
    if (std::strcmp(name, "xrCreateVulkanInstanceKHR") == 0) *out = reinterpret_cast<PFN_xrVoidFunction>(::CreateInstance);
    else if (std::strcmp(name, "xrCreateVulkanDeviceKHR") == 0) *out = reinterpret_cast<PFN_xrVoidFunction>(CreateDevice);
    else if (std::strcmp(name, "xrGetVulkanGraphicsDevice2KHR") == 0) *out = reinterpret_cast<PFN_xrVoidFunction>(Physical);
    else
#endif
    *out = reinterpret_cast<PFN_xrVoidFunction>(Requirements);
    return true;
}
bool OpenXRRuntime::CreateSession(const void*) {
    m_session = reinterpret_cast<XrSession>(this);
    m_session_running = true;
    ++m_session_run_serial;
    return true;
}
void OpenXRRuntime::DestroySession() { m_session_running = false; compositor_open = false; }
void OpenXRRuntime::ObserveResult(XrResult) noexcept {}
OpenXREventStatus OpenXRRuntime::PollEvents() {
    if (change_space) { ++m_reference_space_change.serial; change_space = false; }
    if (restart_session) { ++m_session_run_serial; m_session_running = true; restart_session = false; }
    if (stop_session) { m_session_running = false; stop_session = false; }
    return OpenXREventStatus::Continue;
}
OpenXRFrameStatus OpenXRRuntime::WaitFrame(OpenXRFrame& frame) {
    Require(m_frame_phase == FramePhase::Idle);
    if (!m_session_running) return OpenXRFrameStatus::SessionNotRunning;
    frame = {};
    frame.serial = m_next_frame_serial++;
    frame.predicted_display_time = frame.serial * 11'111'111;
    frame.predicted_display_period = 11'111'111;
    frame.should_render = should_render;
    m_active_frame_serial = frame.serial;
    m_frame_phase = FramePhase::Waited;
    return OpenXRFrameStatus::Ready;
}
bool OpenXRRuntime::BeginFrame(const OpenXRFrame& frame) {
    Require(m_frame_phase == FramePhase::Waited && frame.serial == m_active_frame_serial);
    m_frame_phase = FramePhase::Begun;
    compositor_open = true;
    return true;
}
bool OpenXRRuntime::LocateViews(OpenXRFrame& frame) {
    frame.views_valid = tracking_valid;
    frame.view_state_flags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    for (auto& view : frame.views) {
        view.pose.orientation.w = 1;
        view.pose.position.x = static_cast<float>(frame.serial);
        view.fov.angleLeft = -0.75f;
    }
    return true;
}
bool OpenXRRuntime::LocateViewsAt(XrTime time, OpenXRFrame& frame) {
    if (expect_render_first) Require(!compositor_open);
    frame.predicted_display_time = time;
    return LocateViews(frame);
}
bool OpenXRRuntime::EndFrame(const OpenXRFrame& frame,
                             const XrCompositionLayerBaseHeader* const* layers, uint32_t count) {
    Require(m_frame_phase == FramePhase::Begun && frame.serial == m_active_frame_serial);
    Require(frame.predicted_display_time > display_time);
    display_time = frame.predicted_display_time;
    layer_count = count;
    panel_content = 0;
    if (count) {
        Require(frame.should_render && count <= 2);
        layer_type = layers[0]->type;
        const auto check_image = [](const XrSwapchainSubImage& subimage) {
            const auto& chain = *reinterpret_cast<Swapchain*>(subimage.swapchain);
            Require(chain.released && !chain.acquired && chain.image.content != 0);
            return chain.image.content;
        };
        if (layer_type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(layers[0]);
            Require(projection.viewCount == 2);
            for (const auto& view : {projection.views[0], projection.views[1]}) {
                displayed_content = check_image(view.subImage);
                // Detect a new image paired with an old pose, or a repeated image
                // falsely labelled with the latest compositor pose.
                Require(view.pose.position.x == static_cast<float>(displayed_content));
                Require(view.fov.angleLeft == -0.75f);
            }
        } else {
            Require(layer_type == XR_TYPE_COMPOSITION_LAYER_QUAD);
            const auto& quad = *reinterpret_cast<const XrCompositionLayerQuad*>(layers[0]);
            displayed_content = check_image(quad.subImage);
            quad_pose = quad.pose;
        }
        if (count == 2) {
            // The settings panel, over the scene.
            Require(layers[1]->type == XR_TYPE_COMPOSITION_LAYER_QUAD);
            const auto& panel = *reinterpret_cast<const XrCompositionLayerQuad*>(layers[1]);
            Require(panel.layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
            Require(panel.subImage.imageRect.extent.width == static_cast<int32_t>(kOpenXRPanelLayerWidth));
            Require(panel.subImage.imageRect.extent.height == static_cast<int32_t>(kOpenXRPanelLayerHeight));
            panel_content = check_image(panel.subImage);
            panel_pose = panel.pose;
            panel_size = panel.size;
        }
    }
    m_frame_phase = FramePhase::Idle;
    compositor_open = false;
    if (fail_end) {
        fail_end = false;
        return false;
    }
    return true;
}
bool OpenXRRuntime::EndFrameWithoutLayers(const OpenXRFrame& frame) {
    return EndFrame(frame, nullptr, 0);
}
}

void TestRenderFirst() {
    display_time = 0;
    expect_render_first = true;
    OpenXRRuntime runtime;
    OpenXRD3D12Backend backend;
    Require(backend.QueryGraphicsRequirements(runtime) && backend.BindAurora(runtime));
    OpenXRPresentation presentation;
    OpenXRBackendFrame packet, frame;
    const auto prepare = [&] {
        Require(backend.PreparePacket(presentation, packet) == OpenXRBeginStatus::Ready);
        Require(!compositor_open && packet.expects_gpu_submission);
    };
    const auto submit = [&] {
        Complete();
        Require(backend.WaitForSubmission(packet, 0) == OpenXRSubmissionStatus::Success);
        Require(backend.BeginFrameForPacket(packet, frame) == OpenXRBeginStatus::Ready);
        Require(compositor_open && frame.xr_frame.serial != packet.xr_frame.serial);
        Require(backend.CopyRenderedEyes(frame) == OpenXRSubmissionStatus::Success);
        Require(backend.FinishFrame(frame, true));
        Require(!compositor_open);
    };
    prepare();
    Require(backend.BeginFrameForPacket(packet, frame) == OpenXRBeginStatus::Error);
    submit();
    const auto first = displayed_content;
    Require(first == packet.xr_frame.serial && layer_count == 1);
    prepare();
    const auto before_stall = releases;
    encoded = true;
    for (int i = 0; i < 300; ++i) {
        Require(backend.WaitForSubmission(packet, 0) == OpenXRSubmissionStatus::Timeout);
        Require(!backend.TryCancelPendingPacket(packet));
        Require(backend.KeepAliveCycle() == OpenXRBeginStatus::Ready);
        Require(!compositor_open && layer_count == 1 && displayed_content == first);
        Require(releases == before_stall);
    }
    submit(); // Must preserve original poses across independently advancing cycles.
    Require(displayed_content == packet.xr_frame.serial);
    const auto second = displayed_content;
    prepare();
    auto stale = packet;
    ++stale.xr_frame.serial;
    Require(!backend.TryCancelPendingPacket(stale));
    Require(backend.BeginFrameForPacket(stale, frame) == OpenXRBeginStatus::Error);
    Require(backend.TryCancelPendingPacket(packet));
    Require(!compositor_open && releases == before_stall + 4);
    Require(backend.KeepAliveCycle() == OpenXRBeginStatus::Ready);
    Require(displayed_content == second);

    // Original frame-first mode still works after switching interpolation on.
    expect_render_first = false;
    Require(backend.BeginFrame(presentation, frame) == OpenXRBeginStatus::Ready);
    Complete();
    Require(backend.FinishFrame(frame, true));
    expect_render_first = true;
    prepare();
    Require(packet.xr_frame.predicted_display_time > display_time);
    Require(backend.TryCancelPendingPacket(packet));
    presentation.mode = OpenXRFrameMode::VirtualScreen;
    presentation.quad_anchored = true;
    presentation.quad_pose.position.z = -4;
    prepare();
    Require(targets.size() == 1);
    submit();
    Require(layer_type == XR_TYPE_COMPOSITION_LAYER_QUAD && quad_pose.position.z == -4);

    presentation.mode = OpenXRFrameMode::ImmersiveProjection;
    prepare();
    should_render = false; // Visibility can change while rendering.
    submit();
    Require(layer_count == 0);
    Require(backend.PreparePacket(presentation, packet) == OpenXRBeginStatus::Ready);
    Require(!packet.expects_gpu_submission);
    should_render = true;
    Require(backend.KeepAliveCycle() == OpenXRBeginStatus::Ready);
    tracking_valid = false;
    Require(backend.PreparePacket(presentation, packet) == OpenXRBeginStatus::Ready);
    Require(!packet.expects_gpu_submission);
    tracking_valid = true;

    for (bool session_change : {false, true}) {
        prepare();
        change_space = !session_change;
        restart_session = session_change;
        runtime.PollEvents();
        submit();
        Require(layer_count == 0); // Do not relabel old images with new space/session serials.
        prepare();
        submit();
        Require(layer_count == 1);
    }
    prepare();
    Complete();
    stop_session = true;
    runtime.PollEvents();
    Require(backend.BeginFrameForPacket(packet, frame) == OpenXRBeginStatus::SessionNotRunning);
    restart_session = true;
    runtime.PollEvents();
    prepare();
    submit();
    Require(layer_count == 1);

    prepare();
    const auto before_failure = releases;
    Complete(false);
    Require(backend.WaitForSubmission(packet, 0) == OpenXRSubmissionStatus::Failed);
    Require(backend.BeginFrameForPacket(packet, frame) == OpenXRBeginStatus::Error);
    Require(releases == before_failure);
    Require(backend.Shutdown() && live_swapchains == 0);
    // Shutdown must also drain packets that never entered a compositor frame,
    // including a runtime end failure or session stop during a keep-alive.
    for (int scenario = 0; scenario < 3; ++scenario) {
        display_time = 0;
        OpenXRRuntime next_runtime;
        OpenXRD3D12Backend next_backend;
        Require(next_backend.QueryGraphicsRequirements(next_runtime));
        Require(next_backend.BindAurora(next_runtime));
        Require(next_backend.PreparePacket(presentation, packet) == OpenXRBeginStatus::Ready);
        encoded = true;
        if (scenario == 1) {
            fail_end = true;
            Require(next_backend.KeepAliveCycle() == OpenXRBeginStatus::Error);
        } else if (scenario == 2) {
            stop_session = true;
            next_runtime.PollEvents();
            Require(next_backend.KeepAliveCycle() == OpenXRBeginStatus::SessionNotRunning);
        }
        Require(!compositor_open);
        Require(next_backend.Shutdown() && live_swapchains == 0);
    }
    expect_render_first = false;
    display_time = 0;
}

// The settings panel's quad layer: made when it first opens, rendered with the
// eyes, and shown from the image the last submitted frame wrote, never from one
// a cancelled frame released unwritten.
void TestPanelLayer() {
    display_time = 0;
    OpenXRRuntime runtime;
    OpenXRD3D12Backend backend;
    Require(backend.QueryGraphicsRequirements(runtime) && backend.BindAurora(runtime));
    Require(live_swapchains == 4 && backend.PanelLayerAvailable()); // Nothing is made while it is closed.
    OpenXRPresentation presentation;
    OpenXRBackendFrame frame;
    const auto begin = [&] {
        Require(backend.BeginFrame(presentation, frame) == OpenXRBeginStatus::Ready);
    };
    const auto finish = [&] {
        Complete();
        Require(backend.WaitForSubmission(frame, 0) == OpenXRSubmissionStatus::Success);
        Require(backend.FinishFrame(frame, true));
    };
    begin();
    Require(!has_panel_target);
    finish();
    Require(layer_count == 1);

    presentation.panel.requested = true;
    presentation.panel.placed = true;
    presentation.panel.pose.position.z = -2;
    presentation.panel.width_meters = 1.0f;
    presentation.panel.height_meters = 0.75f;
    begin();
    Require(has_panel_target && live_swapchains == 6);
    Require(panel_target.width == kOpenXRPanelLayerWidth && panel_target.height == kOpenXRPanelLayerHeight);
    finish();
    const auto shown = frame.xr_frame.serial;
    Require(layer_count == 2 && displayed_content == shown && panel_content == shown);
    Require(panel_pose.position.z == -2 && panel_size.width == 1.0f && panel_size.height == 0.75f);

    begin(); // Canceled: its panel image is released unwritten.
    Require(has_panel_target);
    Require(backend.RepeatFrame(frame) && layer_count == 2 && panel_content == shown);
    Require(backend.TryCancelPendingFrame(frame) && backend.FinishFrame(frame, false));
    Require(layer_count == 2 && panel_content == shown);

    presentation.panel.placed = false; // No head pose to hang it from: rendered, not shown.
    begin();
    finish();
    Require(layer_count == 1 && displayed_content == frame.xr_frame.serial);
    presentation.panel.placed = true;
    begin();
    finish();
    Require(layer_count == 2 && panel_content == frame.xr_frame.serial);

    presentation.panel.requested = false; // Closed: neither rendered nor shown, even when repeated.
    begin();
    Require(!has_panel_target);
    finish();
    Require(layer_count == 1);
    begin();
    Require(backend.RepeatFrame(frame) && layer_count == 1);
    Require(backend.TryCancelPendingFrame(frame) && backend.FinishFrame(frame, false));
    Require(layer_count == 1);

    presentation.panel.requested = true; // Reopened on the same swapchains.
    begin();
    finish();
    Require(layer_count == 2 && live_swapchains == 6 && panel_content == frame.xr_frame.serial);

    expect_render_first = true;
    OpenXRBackendFrame packet;
    Require(backend.PreparePacket(presentation, packet) == OpenXRBeginStatus::Ready);
    Require(has_panel_target && packet.presentation.panel.requested);
    Complete();
    Require(backend.WaitForSubmission(packet, 0) == OpenXRSubmissionStatus::Success);
    Require(backend.BeginFrameForPacket(packet, frame) == OpenXRBeginStatus::Ready);
    Require(backend.CopyRenderedEyes(frame) == OpenXRSubmissionStatus::Success);
    Require(backend.FinishFrame(frame, true));
    Require(layer_count == 2 && panel_content == packet.xr_frame.serial);
    expect_render_first = false;
    Require(backend.Shutdown() && live_swapchains == 0);
    display_time = 0;
}

bool SameRect(const XrRect2Di& rect, int32_t x, int32_t y, int32_t width, int32_t height) {
    return rect.offset.x == x && rect.offset.y == y && rect.extent.width == width && rect.extent.height == height;
}

// The menu quad shows only the part of its eye-sized image that Aurora draws into.
void TestVirtualScreenContentRect() {
    // A Quest 3 eye at render_scale 0.8 holding the 1280x720 snapshot: the bands above and below
    // go, and the in-eye settings panel (3/4 of the width, 4:3) is exactly as tall as what is left.
    const XrRect2Di quest = OpenXRVirtualScreenContentRect(1344, 1408, 16.0f / 9.0f);
    Require(SameRect(quest, 0, 326, 1344, 756));
    // At the whole image's size per pixel, the cropped quad is as tall as the snapshot the pointer
    // maps onto (MenuPictureHalfExtents, 2.4 m across).
    const float quad_height = 2.4f * static_cast<float>(quest.extent.height) / 1344.0f;
    const auto picture = wii_remote::MenuPictureHalfExtents(2.4f, 1344.0f / 1408.0f, 16.0f / 9.0f, 16.0f / 9.0f);
    Require(std::fabs(quad_height - 2.0f * picture[1]) < 1e-4f);
    // A snapshot narrower than the image is pillarboxed; the panel still has to fit.
    Require(SameRect(OpenXRVirtualScreenContentRect(1000, 1000, 0.5f), 125, 0, 750, 1000));
    // A wide snapshot leaves the panel taller than the picture: keep the panel whole.
    Require(SameRect(OpenXRVirtualScreenContentRect(1344, 1408, 2.4f), 0, 326, 1344, 756));
    // Before Aurora has published an aspect, the whole image.
    Require(SameRect(OpenXRVirtualScreenContentRect(1344, 1408, 0.0f), 0, 0, 1344, 1408));
}

#if defined(TEST_WINDOWS_VULKAN)
// VR interpolation's per-refresh eye sets (inline and pipelined pacing): a set is shown with
// its own poses, only after it completed; a late set leaves the retained layer on display
// and is shown by a later cycle; the two swapchain pairs alternate so no swapchain ever has
// two images acquired (the stub requires it).
void TestEyeSets() {
    display_time = 0;
    OpenXRRuntime runtime;
    OpenXRD3D12Backend backend;
    Require(backend.QueryGraphicsRequirements(runtime) && backend.BindAurora(runtime));
    OpenXRPresentation presentation;
    OpenXRFrame cycle;
    OpenXRBackendFrame set;
    const auto now = [] { return std::chrono::steady_clock::now(); };

    // Inline: located for this cycle's display time, shown by it.
    for (int i = 0; i < 4; ++i) {
        Require(backend.BeginCycle(cycle) == OpenXRBeginStatus::Ready && compositor_open);
        Require(backend.PrepareSet(presentation, cycle.predicted_display_time, cycle, set) == OpenXRBeginStatus::Ready);
        Require(set.expects_gpu_submission && set.xr_frame.predicted_display_time == cycle.predicted_display_time);
        Require(backend.WaitForSubmissionUntil(set, now()) == OpenXRSubmissionStatus::Timeout);
        Complete();
        Require(backend.WaitForSubmissionUntil(set, now()) == OpenXRSubmissionStatus::Success);
        const auto before = releases;
        Require(backend.EndCycleWithSet(set) && !compositor_open);
        Require(releases == before + 2 && layer_count == 1 && displayed_content == set.xr_frame.serial);
    }
    const auto shown = displayed_content;

    // Late: the retained layer is repeated, nothing is released, then the set is shown.
    Require(backend.BeginCycle(cycle) == OpenXRBeginStatus::Ready);
    Require(backend.PrepareSet(presentation, cycle.predicted_display_time, cycle, set) == OpenXRBeginStatus::Ready);
    encoded = true;
    const auto before_late = releases;
    Require(backend.WaitForSubmissionUntil(set, now()) == OpenXRSubmissionStatus::Timeout);
    Require(backend.EndCycleRetained() && layer_count == 1 && displayed_content == shown && releases == before_late);
    Require(backend.HasPendingSet());
    Require(backend.BeginCycle(cycle) == OpenXRBeginStatus::Ready);
    Require(backend.WaitForSubmissionUntil(set, now()) == OpenXRSubmissionStatus::Timeout);
    Require(!backend.CancelSet(set));  // Aurora is encoding it
    Require(backend.EndCycleRetained() && displayed_content == shown && releases == before_late);
    Require(backend.BeginCycle(cycle) == OpenXRBeginStatus::Ready);
    Complete();
    Require(backend.WaitForSubmissionUntil(set, now()) == OpenXRSubmissionStatus::Success);
    Require(backend.EndCycleWithSet(set) && displayed_content == set.xr_frame.serial && !backend.HasPendingSet());

    // Pipelined: located for the next cycle, published after this one ended, shown by it.
    Require(backend.BeginCycle(cycle) == OpenXRBeginStatus::Ready);
    Require(backend.EndCycleRetained());
    for (int i = 0; i < 6; ++i) {
        const XrTime next = cycle.predicted_display_time + cycle.predicted_display_period;
        Require(backend.PrepareSet(presentation, next, cycle, set) == OpenXRBeginStatus::Ready && !compositor_open);
        Complete();
        Require(backend.BeginCycle(cycle) == OpenXRBeginStatus::Ready);
        Require(cycle.predicted_display_time == next);
        Require(backend.WaitForSubmissionUntil(set, now()) == OpenXRSubmissionStatus::Success);
        Require(backend.EndCycleWithSet(set) && displayed_content == set.xr_frame.serial);
    }

    // A canceled set keeps its images for the next one; a dropped one is released only
    // after it completed.
    Require(backend.PrepareSet(presentation, cycle.predicted_display_time + 1, cycle, set) == OpenXRBeginStatus::Ready);
    const auto before_cancel = releases;
    Require(backend.CancelSet(set) && !backend.HasPendingSet() && releases == before_cancel);
    Require(backend.PrepareSet(presentation, cycle.predicted_display_time + 2, cycle, set) == OpenXRBeginStatus::Ready);
    Complete();
    const auto shown_last = displayed_content;
    Require(backend.DropSet(set) && !backend.HasPendingSet());
    Require(backend.BeginCycle(cycle) == OpenXRBeginStatus::Ready);
    Require(backend.EndCycleRetained() && displayed_content == shown_last);
    Require(backend.Shutdown() && live_swapchains == 0);
    display_time = 0;
}
#endif

int main() {
#if defined(TEST_WINDOWS_VULKAN)
    TestEyeSets();
#endif
    TestRenderFirst();
    TestPanelLayer();
    TestVirtualScreenContentRect();
    OpenXRRuntime runtime;
    OpenXRD3D12Backend backend;
    Require(backend.QueryGraphicsRequirements(runtime) && backend.BindAurora(runtime));
    Require(live_swapchains == 4);
    OpenXRD3D12Presentation presentation;
    OpenXRD3D12Frame frame;
    const auto begin = [&] {
        Require(backend.BeginFrame(presentation, frame) == OpenXRD3D12BeginStatus::Ready);
    };
    const auto finish = [&] {
        Complete();
        Require(backend.WaitForSubmission(frame, 0) == OpenXRD3D12SubmissionStatus::Success);
        Require(backend.FinishFrame(frame, true));
    };
    begin();
    Require(backend.RepeatFrame(frame) && layer_count == 0); // No valid image yet.
    finish();
    const auto first = displayed_content;
    begin();
    encoded = true;
    const auto releases_before_stall = releases;
    for (int i = 0; i < 300; ++i) {
        Require(backend.WaitForSubmission(frame, 0) == OpenXRD3D12SubmissionStatus::Timeout);
        Require(!backend.TryCancelPendingFrame(frame));
        Require(backend.RepeatFrame(frame));
        Require(layer_count == 1 && displayed_content == first);
        Require(releases == releases_before_stall);
    }
    const auto second = frame.xr_frame.serial;
    finish(); // A late completion keeps its original render token and poses.
    Require(displayed_content == second);
    begin();
    Require(backend.TryCancelPendingFrame(frame));
    Require(backend.FinishFrame(frame, false));
    Require(layer_count == 1 && displayed_content == second);

    should_render = false;
    begin();
    Require(!frame.expects_gpu_submission && backend.FinishFrame(frame, false));
    Require(layer_count == 0);
    should_render = true;
    tracking_valid = false;
    begin();
    Require(!frame.expects_gpu_submission && backend.FinishFrame(frame, false));
    Require(layer_count == 1 && displayed_content == second);
    tracking_valid = true;

    presentation.mode = OpenXRD3D12FrameMode::VirtualScreen;
    presentation.quad_anchored = true;
    presentation.quad_pose.position.z = -3;
    begin();
    Require(targets.size() == 1);
    finish();
    const auto menu = displayed_content;
    begin();
    Require(backend.RepeatFrame(frame));
    Require(displayed_content == menu && quad_pose.position.z == -3);
    Require(backend.TryCancelPendingFrame(frame) && backend.FinishFrame(frame, false));

    presentation.mode = OpenXRD3D12FrameMode::ImmersiveProjection;
    begin();
    change_space = true;
    Require(backend.RepeatFrame(frame));
    Require(backend.RepeatFrame(frame) && layer_count == 0);
    finish(); // A render from before the space change must remain invalid.
    Require(layer_count == 0);
    begin();
    finish();
    Require(layer_count == 1);
    restart_session = true;
    runtime.PollEvents();
    begin();
    Require(backend.RepeatFrame(frame) && layer_count == 0);
    finish();
    Require(layer_count == 1);

    begin();
    const auto before_failure = releases;
    Complete(false);
    Require(backend.WaitForSubmission(frame, 0) == OpenXRD3D12SubmissionStatus::Failed);
    Require(!backend.FinishFrame(frame, false));
    Require(releases == before_failure); // Never release possibly in-flight GPU work.
    Require(backend.Shutdown() && live_swapchains == 0);

    // A runtime stop or failed xrEndFrame during a stall must still drain the
    // pending target without trying to end an already consumed frame token.
    for (bool fail_submission : {false, true}) {
        display_time = 0;
        OpenXRRuntime next_runtime;
        OpenXRD3D12Backend next_backend;
        Require(next_backend.QueryGraphicsRequirements(next_runtime));
        Require(next_backend.BindAurora(next_runtime));
        Require(next_backend.BeginFrame(presentation, frame) == OpenXRD3D12BeginStatus::Ready);
        encoded = true;
        stop_session = !fail_submission;
        fail_end = fail_submission;
        Require(!next_backend.RepeatFrame(frame));
        Require(next_backend.Shutdown() && live_swapchains == 0);
    }
    std::cout << "OpenXR retained-frame tests passed\n";
}
