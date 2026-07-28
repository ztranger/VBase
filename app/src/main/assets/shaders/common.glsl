// Shared frame data (used by every program that needs camera/light).
// std140 layout — MUST match the FrameUBO struct in GlRenderer.cpp.
layout(std140) uniform Frame {
    mat4 uViewProj;
    vec3 uLightDir;
    vec3 uViewPos;
};
