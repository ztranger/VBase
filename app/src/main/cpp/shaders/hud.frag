#version 450

// Vulkan HUD fragment shader: альфа глифа из атласа шрифта, цвет из push.

layout(location = 0) in vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2D uFont;

layout(push_constant) uniform Push {
    vec4 uScreen;
    vec4 uColor;
} pc;

layout(location = 0) out vec4 fragColor;

void main() {
    float a = texture(uFont, vUV).a;
    fragColor = vec4(pc.uColor.rgb, a);
}
