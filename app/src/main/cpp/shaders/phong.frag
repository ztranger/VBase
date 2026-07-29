#version 450

// Vulkan Phong fragment shader: диффуз + зеркальный блик по позиции камеры.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;

layout(set = 0, binding = 0) uniform Frame {
    mat4 uViewProj;
    vec4 uLightDir;
    vec4 uViewPos;
} frame;

layout(set = 1, binding = 0) uniform sampler2D uAlbedo;

layout(push_constant) uniform Push {
    vec4 uColor;
} pc;

layout(location = 0) out vec4 fragColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(frame.uLightDir.xyz);
    vec3 V = normalize(frame.uViewPos.xyz - vWorldPos);
    vec3 R = reflect(-L, N);
    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(V, R), 0.0), 32.0);
    vec3 albedo = texture(uAlbedo, vUV).rgb * pc.uColor.rgb;
    vec3 c = albedo * (0.2 + 0.8 * diff) + vec3(1.0) * (spec * 0.5);
    fragColor = vec4(c, 1.0);
}
