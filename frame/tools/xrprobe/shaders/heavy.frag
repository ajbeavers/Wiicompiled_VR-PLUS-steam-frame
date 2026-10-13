#version 450
// xrprobe: calibrated GPU load. aux[0] = (iterations, hue 0..1, k, unused).
// Writes the dim hue background of the pace cases, so a frame the compositor
// reads before this pass finishes shows the wrong background (old frame or
// tiles of it). k is 0 at run time; it keeps the loop from being optimised out.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
vec3 hueRgb(float h) {
    return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}
void main() {
    vec4 prm = pc.aux[0];
    int n = int(prm.x);
    float acc = gl_FragCoord.x * 0.00123 + gl_FragCoord.y * 0.00071;
    for (int i = 0; i < n; i++) acc = fract(sin(acc * 12.9898 + float(i) * 0.0137) * 43758.5453);
    vec3 col = hueRgb(prm.y) * 0.12 + 0.02;
    outColor = vec4(col + acc * prm.z, 1.0);
}
