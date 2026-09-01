// Shadow pass (skinned casters): same bone skinning as skin.vert, projected by
// the light. Reuses the bone texture (unit 1) filled once per frame.
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 aJoints;
layout(location = 4) in vec4 aWeights;
#include "common.glsl"
uniform mat4 uModel;
uniform highp sampler2D uBones;
uniform int uBoneOffset;
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
    gl_Position = uLightVP * uModel * skin * vec4(aPos, 1.0);
}
