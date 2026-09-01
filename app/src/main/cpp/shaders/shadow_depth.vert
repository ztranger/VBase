#version 450

// Vulkan shadow pass (instanced static geometry): position only, projected by the
// light. Depth-only pipeline (no fragment stage). Same vertex/instance layout as
// lit.vert, so iModel is at locations 3..6.

layout(location = 0) in vec3 aPos;
layout(location = 3) in mat4 iModel;  // per-instance (локации 3,4,5,6)

layout(set = 0, binding = 0) uniform Frame {
    mat4 uViewProj;
    mat4 uLightVP;
    vec4 uLightDir;
    vec4 uViewPos;
    vec4 uFogColor;
} frame;

void main() {
    gl_Position = frame.uLightVP * iModel * vec4(aPos, 1.0);
}
