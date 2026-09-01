// Lit: Lambert diffuse + texture, hemispheric ambient, shadow, gamma-correct.
#include "common.glsl"
#include "lighting.glsl"
in vec3 vNormal;
in vec2 vUV;
in vec3 vWorldPos;
in vec3 vTangent;
in highp vec4 vLightClip;
uniform vec3 uColor;
uniform sampler2D uAlbedo;
out vec4 fragColor;
void main() {
    vec3 N = mapNormal(vNormal, vTangent, vUV);  // нормаль из нормал-карты (TBN)
    float ndotl = max(dot(N, normalize(uLightDir)), 0.0);
    float sh = shadowFactor(vLightClip, ndotl);
    vec3 albedo = toLinear(texture(uAlbedo, vUV).rgb * uColor);  // sRGB -> linear
    vec3 c = albedo * (hemiAmbient(N) + kSunColor * ndotl * sh);
    c = applyFog(c, vWorldPos);
    fragColor = vec4(toGamma(c), 1.0);  // linear -> sRGB
}
