// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aurora/dawn_vulkan_abi.h>
#include <aurora/d3d12_interop.h>
#ifdef __cplusplus
extern "C" {
#endif
// Same pending-target/callback contract as D3D12. resource carries a VkImage
// encoded as a pointer-sized value; colorDxgiFormat carries a VkFormat.
bool aurora_vulkan_win32_configure(const AuroraDawnVulkanHooks* hooks);
bool aurora_vulkan_win32_get_handles(AuroraDawnVulkanHandles* handles, int64_t* colorFormat);
bool aurora_vulkan_win32_enable(AuroraD3D12StereoSubmittedCallback submitted, void* userdata);
bool aurora_vulkan_win32_set_targets(uint64_t token, const AuroraD3D12StereoTarget* targets, uint32_t count);
// Plus the headset settings panel's quad-layer image when panel is not null, as
// aurora_d3d12_set_stereo_targets_with_panel.
bool aurora_vulkan_win32_set_targets_with_panel(uint64_t token, const AuroraD3D12StereoTarget* targets, uint32_t count,
                                                const AuroraD3D12StereoTarget* panel);
bool aurora_vulkan_win32_cancel(uint64_t token);
bool aurora_vulkan_win32_disable();
// Only inside the submitted callback, on the frame worker: true when the set being
// reported was rendered from a retained game frame (VR frame interpolation, the
// projected menu) instead of a newly sealed one.
bool aurora_vulkan_win32_submitting_retained();
void* aurora_vulkan_win32_lock_queue();
void aurora_vulkan_win32_unlock_queue(void* guard);
#ifdef __cplusplus
}
#endif
