// Shared vertex shader for Lit/Unlit. viewProj from UBO, model per-instance.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in mat4 iModel;  // instance matrix (locations 3..6)
#include "common.glsl"
out vec3 vNormal;
out vec2 vUV;
out vec3 vWorldPos;         // world position (fog distance)
out highp vec4 vLightClip;  // position in light clip space (shadow lookup)
void main() {
    vec4 world = iModel * vec4(aPos, 1.0);
    gl_Position = uViewProj * world;
    vNormal = mat3(iModel) * aNormal;
    vUV = aUV;
    vWorldPos = world.xyz;
    vLightClip = uLightVP * world;
}
