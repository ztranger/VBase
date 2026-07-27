#version 450

// Тот же push-блок, что и в вершинном шейдере (диапазон покрывает обе стадии).
layout(push_constant) uniform Push {
    float uAngle;
    float uAspect;
    vec2  uCenter;
    float uPressed;
} pc;

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 c = mix(vColor, vec3(1.0), 0.4 * pc.uPressed);  // подсветка при касании
    fragColor = vec4(c, 1.0);
}
