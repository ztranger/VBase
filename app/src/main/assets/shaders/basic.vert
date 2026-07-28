// Shared vertex shader for Lit/Unlit. viewProj from UBO, model per-instance.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in mat4 iModel;  // instance matrix (locations 3..6)
#include "common.glsl"
out vec3 vNormal;
out vec2 vUV;
void main() {
    gl_Position = uViewProj * iModel * vec4(aPos, 1.0);
    vNormal = mat3(iModel) * aNormal;
    vUV = aUV;
}
