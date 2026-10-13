// xrprobe: push constants shared by every pipeline.
// mvp: model-view-projection of the current frame (Vulkan clip, y down, z 0..1).
// aux: scene/heavy passes read aux[0] as parameters; the motion vector pass
//      reads aux as the previous frame's model-view-projection.
layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 aux;
} pc;
