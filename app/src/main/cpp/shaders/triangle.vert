#version 450

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;

// Параметры кадра идут через push-константы (без descriptor set'ов).
// Раскладка совпадает с C-структурой PushConstants в VulkanRenderer.cpp.
layout(push_constant) uniform Push {
    float uAngle;
    float uAspect;
    vec2  uCenter;  // центр треугольника в NDC (следует за пальцем)
    float uPressed;
} pc;

layout(location = 0) out vec3 vColor;

void main() {
    float c = cos(pc.uAngle);
    float s = sin(pc.uAngle);
    vec2 p = mat2(c, s, -s, c) * aPos;
    p.x /= pc.uAspect;                       // компенсируем соотношение сторон
    gl_Position = vec4(p + pc.uCenter, 0.0, 1.0);
    vColor = aColor;
}
