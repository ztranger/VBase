// Phong: diffuse + specular highlight based on camera position.
#include "common.glsl"
in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vUV;
uniform vec3 uColor;
uniform sampler2D uAlbedo;
out vec4 fragColor;
void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 R = reflect(-L, N);
    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(V, R), 0.0), 32.0);
    vec3 albedo = texture(uAlbedo, vUV).rgb * uColor;
    vec3 c = albedo * (0.2 + 0.8 * diff) + vec3(1.0) * (spec * 0.5);
    fragColor = vec4(c, 1.0);
}
