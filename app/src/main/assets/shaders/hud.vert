// HUD: 2D text. Positions come in pixels; shader maps them to NDC.
layout(location = 0) in vec2 aPos;  // pixels
layout(location = 1) in vec2 aUV;
uniform vec2 uScreen;
out vec2 vUV;
void main() {
    vec2 ndc = vec2(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
}
