// Shared frame data (used by every program that needs camera/light).
// std140 layout — MUST match the FrameUBO struct in GlRenderer.cpp.
layout(std140) uniform Frame {
    mat4 uViewProj;   // 0
    mat4 uLightVP;    // 64  view-proj source (directional), for shadow lookup
    vec3 uLightDir;   // 128 direction TO the light
    vec3 uViewPos;    // 144 camera position (Phong)
    float uShadowBias;// 156 depth bias against self-shadow acne
};
