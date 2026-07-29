#version 450

// Vulkan Lit fragment shader: Lambert diffuse * (texture * base color).

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;

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
    float diff = max(dot(N, normalize(frame.uLightDir.xyz)), 0.0);
    vec3 albedo = texture(uAlbedo, vUV).rgb * pc.uColor.rgb;
    fragColor = vec4(albedo * (0.25 + 0.75 * diff), 1.0);
}
