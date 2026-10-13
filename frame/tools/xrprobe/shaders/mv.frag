#version 450
// xrprobe: motion vectors for XR_EXT_frame_synthesis and XR_FB_space_warp
// (both take (CurrNDC - PrevNDC).xyz of the content, head motion excluded) in
// Vulkan NDC ([-1,-1,0]..[1,1,1]); alpha is ignored by the runtime.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vCur;
layout(location = 2) in vec4 vPrev;
layout(location = 0) out vec4 outMv;
void main() {
    vec3 cur = vCur.xyz / vCur.w;
    vec3 prev = vPrev.xyz / vPrev.w;
    outMv = vec4(cur - prev, 0.0);
}
