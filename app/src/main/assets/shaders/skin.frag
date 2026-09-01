// Skinned mesh: same Lambert diffuse + texture as Lit (ambient + shadow + gamma).
#include "common.glsl"
#include "lighting.glsl"
in vec3 vNormal;
in vec2 vUV;
in highp vec4 vLightClip;
uniform vec3 uColor;
uniform sampler2D uAlbedo;
out vec4 fragColor;
void main() {
    vec3 N = normalize(vNormal);
    float ndotl = max(dot(N, normalize(uLightDir)), 0.0);
    float sh = shadowFactor(vLightClip, ndotl);
    vec3 albedo = toLinear(texture(uAlbedo, vUV).rgb * uColor);
    vec3 c = albedo * (hemiAmbient(N) + kSunColor * ndotl * sh);
    fragColor = vec4(toGamma(c), 1.0);
}
