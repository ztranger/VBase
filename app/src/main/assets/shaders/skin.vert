// Skinning: position is a weighted sum of bone matrices.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aJoints;
layout(location = 4) in vec4 aWeights;
#include "common.glsl"
uniform mat4 uModel;
uniform highp sampler2D uBones;  // bones: row = bone, 4 texels = 4 columns
uniform int uBoneOffset;         // this model's bone offset in the texture
out vec3 vNormal;
out vec2 vUV;
out vec3 vWorldPos;         // world position (fog distance)
out highp vec4 vLightClip;  // position in light clip space (shadow lookup)
mat4 boneMat(int j) {
    int row = uBoneOffset + j;
    return mat4(texelFetch(uBones, ivec2(0, row), 0),
               texelFetch(uBones, ivec2(1, row), 0),
               texelFetch(uBones, ivec2(2, row), 0),
               texelFetch(uBones, ivec2(3, row), 0));
}
void main() {
    mat4 skin = aWeights.x * boneMat(int(aJoints.x))
              + aWeights.y * boneMat(int(aJoints.y))
              + aWeights.z * boneMat(int(aJoints.z))
              + aWeights.w * boneMat(int(aJoints.w));
    vec4 worldPos = uModel * skin * vec4(aPos, 1.0);
    gl_Position = uViewProj * worldPos;
    vNormal = mat3(uModel * skin) * aNormal;
    vUV = aUV;
    vWorldPos = worldPos.xyz;
    vLightClip = uLightVP * worldPos;
}
