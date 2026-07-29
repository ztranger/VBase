#version 450

// Vulkan Unlit fragment shader: flat texture * base color, no lighting.

layout(location = 0) in vec3 vNormal;   // не используется, но приходит из lit.vert
layout(location = 1) in vec2 vUV;

layout(set = 1, binding = 0) uniform sampler2D uAlbedo;

layout(push_constant) uniform Push {
    vec4 uColor;
} pc;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vec4(texture(uAlbedo, vUV).rgb * pc.uColor.rgb, 1.0);
}
