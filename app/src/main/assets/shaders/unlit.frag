// Unlit: flat color/texture, no lighting (Frame block not needed here).
in vec3 vNormal;
in vec2 vUV;
uniform vec3 uColor;
uniform sampler2D uAlbedo;
out vec4 fragColor;
void main() {
    fragColor = vec4(texture(uAlbedo, vUV).rgb * uColor, 1.0);
}
