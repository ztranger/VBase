#version 450

// Vulkan HUD vertex shader: позиции в пикселях -> NDC. В Vulkan Y направлен
// вниз (NDC -1 сверху), пиксели тоже сверху вниз, поэтому инверсии Y НЕ нужно
// (в отличие от GL-версии).

layout(location = 0) in vec2 aPos;  // пиксели
layout(location = 1) in vec2 aUV;

layout(push_constant) uniform Push {
    vec4 uScreen;  // xy = размер экрана в пикселях
    vec4 uColor;   // rgb = цвет текста
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    vec2 ndc = vec2(aPos.x / pc.uScreen.x * 2.0 - 1.0,
                    aPos.y / pc.uScreen.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
}
