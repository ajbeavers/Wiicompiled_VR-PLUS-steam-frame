#version 450
// xrprobe: a unit quad in the XY plane, placed by pc.mvp.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vCur;
layout(location = 2) out vec4 vPrev;
const vec2 kPos[6] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
                            vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
void main() {
    vec2 p = kPos[gl_VertexIndex];
    vUv = vec2(p.x + 0.5, 0.5 - p.y); // v = 0 at the top edge
    vCur = pc.mvp * vec4(p, 0.0, 1.0);
    vPrev = pc.aux * vec4(p, 0.0, 1.0);
    gl_Position = vCur;
}
