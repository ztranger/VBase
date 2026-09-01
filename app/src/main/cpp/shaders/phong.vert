#version 450

// Vulkan Phong vertex shader: как lit.vert (инстансная матрица iModel), но
// дополнительно отдаёт мировую позицию для вектора взгляда.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in mat4 iModel;  // per-instance (локации 3,4,5,6)
layout(location = 7) in vec3 aTangent;

layout(set = 0, binding = 0) uniform Frame {
    mat4 uViewProj;
    mat4 uLightVP;
    vec4 uLightDir;
    vec4 uViewPos;
    vec4 uFogColor;
} frame;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out vec4 vLightClip;
layout(location = 4) out vec3 vTangent;

void main() {
    vec4 world = iModel * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    gl_Position = frame.uViewProj * world;
    vNormal = mat3(iModel) * aNormal;
    vUV = aUV;
    vLightClip = frame.uLightVP * world;
    vTangent = mat3(iModel) * aTangent;
}
