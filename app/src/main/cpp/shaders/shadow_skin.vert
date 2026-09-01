#version 450

// Vulkan shadow pass (skinned casters): same bone skinning as skin.vert, projected
// by the light. Depth-only pipeline (no fragment stage). Bones in SSBO (set 2),
// bone offset in push constant.

layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 aJoints;
layout(location = 4) in vec4 aWeights;

layout(set = 0, binding = 0) uniform Frame {
    mat4 uViewProj;
    mat4 uLightVP;
    vec4 uLightDir;
    vec4 uViewPos;
    vec4 uFogColor;
} frame;

layout(set = 2, binding = 0) readonly buffer Bones {
    mat4 bones[];
};

layout(push_constant) uniform Push {
    mat4 uModel;
    vec4 uColor;
    int uBoneOffset;
} pc;

mat4 boneMat(int j) { return bones[pc.uBoneOffset + j]; }

void main() {
    mat4 skin = aWeights.x * boneMat(int(aJoints.x))
              + aWeights.y * boneMat(int(aJoints.y))
              + aWeights.z * boneMat(int(aJoints.z))
              + aWeights.w * boneMat(int(aJoints.w));
    gl_Position = frame.uLightVP * pc.uModel * skin * vec4(aPos, 1.0);
}
