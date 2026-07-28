// Lit: Lambert diffuse + texture.
#include "common.glsl"
in vec3 vNormal;
in vec2 vUV;
uniform vec3 uColor;
uniform sampler2D uAlbedo;
out vec4 fragColor;
void main() {
    vec3 N = normalize(vNormal);
    float diff = max(dot(N, normalize(uLightDir)), 0.0);
    vec3 albedo = texture(uAlbedo, vUV).rgb * uColor;
    fragColor = vec4(albedo * (0.25 + 0.75 * diff), 1.0);
}
