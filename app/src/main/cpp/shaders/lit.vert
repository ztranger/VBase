#version 450

// Vulkan Lit vertex shader. Модельная матрица — инстансный атрибут iModel
// (локации 3..6, из инстанс-буфера), поэтому один drawIndexed рисует много
// копий. Frame — через descriptor set 0. Коррекция клипа зашита в uViewProj.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in mat4 iModel;  // per-instance (локации 3,4,5,6)

layout(set = 0, binding = 0) uniform Frame {
    mat4 uViewProj;
    vec4 uLightDir;
    vec4 uViewPos;
} frame;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;

void main() {
    gl_Position = frame.uViewProj * iModel * vec4(aPos, 1.0);
    vNormal = mat3(iModel) * aNormal;
    vUV = aUV;
}
