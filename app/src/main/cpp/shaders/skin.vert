#version 450

// Vulkan skinning vertex shader. Матрицы костей — в storage-буфере (set 2),
// смещение костей модели — в push-константе uBoneOffset. Позиция = взвешенная
// сумма 4 костей, затем uModel и uViewProj (с коррекцией клипа).

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aJoints;   // индексы костей (как float)
layout(location = 4) in vec4 aWeights;  // веса

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

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out vec4 vLightClip;

mat4 boneMat(int j) { return bones[pc.uBoneOffset + j]; }

void main() {
    mat4 skin = aWeights.x * boneMat(int(aJoints.x))
              + aWeights.y * boneMat(int(aJoints.y))
              + aWeights.z * boneMat(int(aJoints.z))
              + aWeights.w * boneMat(int(aJoints.w));
    mat4 world = pc.uModel * skin;
    vec4 worldPos = world * vec4(aPos, 1.0);
    gl_Position = frame.uViewProj * worldPos;
    vNormal = mat3(world) * aNormal;
    vUV = aUV;
    vWorldPos = worldPos.xyz;
    vLightClip = frame.uLightVP * worldPos;
}
