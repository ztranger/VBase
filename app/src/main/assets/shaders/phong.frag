// Phong: diffuse + specular, hemispheric ambient, shadow, gamma-correct.
#include "common.glsl"
#include "lighting.glsl"
in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vUV;
in highp vec4 vLightClip;
uniform vec3 uColor;
uniform sampler2D uAlbedo;
out vec4 fragColor;
void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 R = reflect(-L, N);
    float ndotl = max(dot(N, L), 0.0);
    float sh = shadowFactor(vLightClip, ndotl);
    vec3 albedo = toLinear(texture(uAlbedo, vUV).rgb * uColor);
    float spec = pow(max(dot(V, R), 0.0), 32.0) * sh;
    vec3 c = albedo * (hemiAmbient(N) + kSunColor * ndotl * sh) + vec3(0.5) * spec;
    c = applyFog(c, vWorldPos);
    fragColor = vec4(toGamma(c), 1.0);
}
