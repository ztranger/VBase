// HUD text: sample the font atlas alpha, tint by uColor.
in vec2 vUV;
uniform sampler2D uFont;
uniform vec3 uColor;
out vec4 fragColor;
void main() {
    float a = texture(uFont, vUV).a;
    fragColor = vec4(uColor, a);
}
