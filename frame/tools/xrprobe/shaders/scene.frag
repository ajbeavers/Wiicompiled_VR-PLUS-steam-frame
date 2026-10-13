#version 450
// xrprobe: colour pass for the quad.
// aux[0] = (mode, frame number, hue 0..1, unused)
//   mode 0: 8x8 checkerboard (synth / depth-only cases)
//   mode 1: frame-number panel: hue background, six seven-segment digits,
//           and a 16-cell ticker whose lit cell advances one step per frame
//   mode 2: case label: eight seven-segment cells, masks in aux[1].xyzw, aux[2].xyzw
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vCur;
layout(location = 2) in vec4 vPrev;
layout(location = 0) out vec4 outColor;

vec3 hueRgb(float h) {
    vec3 k = clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return k;
}

// segments a..g as bits 0..6
const int kSeg[10] = int[](0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F);

bool inBox(vec2 p, vec2 lo, vec2 hi) { return all(greaterThanEqual(p, lo)) && all(lessThanEqual(p, hi)); }

bool segLit(vec2 p, int m) {
    float t = 0.07;
    if ((m & 0x01) != 0 && inBox(p, vec2(0.20, 0.06 - t), vec2(0.80, 0.06 + t))) return true; // a
    if ((m & 0x02) != 0 && inBox(p, vec2(0.80 - t, 0.06), vec2(0.80 + t, 0.50))) return true; // b
    if ((m & 0x04) != 0 && inBox(p, vec2(0.80 - t, 0.50), vec2(0.80 + t, 0.94))) return true; // c
    if ((m & 0x08) != 0 && inBox(p, vec2(0.20, 0.94 - t), vec2(0.80, 0.94 + t))) return true; // d
    if ((m & 0x10) != 0 && inBox(p, vec2(0.20 - t, 0.50), vec2(0.20 + t, 0.94))) return true; // e
    if ((m & 0x20) != 0 && inBox(p, vec2(0.20 - t, 0.06), vec2(0.20 + t, 0.50))) return true; // f
    if ((m & 0x40) != 0 && inBox(p, vec2(0.20, 0.50 - t), vec2(0.80, 0.50 + t))) return true; // g
    return false;
}

void main() {
    vec4 prm = pc.aux[0];
    int mode = int(prm.x + 0.5);
    if (mode == 2) {
        int cell = clamp(int(floor(vUv.x * 8.0)), 0, 7);
        vec2 p = vec2(fract(vUv.x * 8.0), vUv.y);
        int m = int((cell < 4 ? pc.aux[1][cell] : pc.aux[2][cell - 4]) + 0.5);
        outColor = vec4(segLit(p, m) ? vec3(1.0, 0.85, 0.1) : vec3(0.06), 1.0);
        return;
    }
    if (mode == 0) {
        ivec2 c = ivec2(floor(vUv * 8.0));
        bool odd = ((c.x + c.y) & 1) != 0;
        vec3 col = odd ? vec3(0.9) : vec3(0.08, 0.10, 0.35);
        // a red edge so the quad's outline is easy to follow
        if (any(lessThan(vUv, vec2(0.01))) || any(greaterThan(vUv, vec2(0.99)))) col = vec3(0.9, 0.1, 0.1);
        outColor = vec4(col, 1.0);
        return;
    }
    uint frame = uint(prm.y + 0.5);
    vec3 bg = hueRgb(prm.z) * 0.8 + 0.1;
    vec3 col = bg;
    if (vUv.y < 0.72) {
        // six digits, most significant first
        float cellW = 1.0 / 6.0;
        int cell = int(floor(vUv.x / cellW));
        vec2 p = vec2(fract(vUv.x / cellW), (vUv.y - 0.04) / 0.64);
        uint place = 1u;
        for (int i = 0; i < 5 - cell; i++) place *= 10u;
        int d = int((frame / place) % 10u);
        if (p.y >= 0.0 && p.y <= 1.0 && segLit(p, kSeg[d])) col = vec3(0.0);
    } else if (vUv.y > 0.78 && vUv.y < 0.96) {
        int cell = int(floor(vUv.x * 16.0));
        bool lit = uint(cell) == (frame % 16u);
        float edge = fract(vUv.x * 16.0);
        col = (edge < 0.08) ? vec3(0.0) : (lit ? vec3(1.0) : bg * 0.35);
    }
    outColor = vec4(col, 1.0);
}
