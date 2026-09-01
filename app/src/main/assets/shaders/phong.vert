// Phong: vertex shader also outputs world position (for the view vector).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in mat4 iModel;  // instance matrix (locations 3..6)
#include "common.glsl"
out vec3 vNormal;
out vec3 vWorldPos;
out vec2 vUV;
out highp vec4 vLightClip;  // position in light clip space (shadow lookup)
void main() {
    vec4 world = iModel * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    gl_Position = uViewProj * world;
    vNormal = mat3(iModel) * aNormal;
    vUV = aUV;
    vLightClip = uLightVP * world;
}
