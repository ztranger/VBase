// Ground: как Lit (Ламберт + текстура + тени + ambient), но ДАЛЬНЯЯ земля плавно тонируется в
// цвет горизонта (uFogColor) по горизонтальной дистанции от камеры. Так край карты сливается с
// фоном без общего тумана — игровая зона (ближе kFadeStart) остаётся резкой. Только для материала
// пола (ShaderType::Ground), пропсы/юниты используют Lit и не тонируются.
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

// Диапазон тинта в мировых юнитах (горизонтальная дистанция от камеры). Игровая зона < kFadeStart.
const float kFadeStart = 55.0;
const float kFadeEnd   = 120.0;

void main() {
    vec3 N = mapNormal(vNormal, vTangent, vUV);
    float ndotl = max(dot(N, normalize(uLightDir)), 0.0);
    float sh = shadowFactor(vLightClip, ndotl);
    vec3 albedo = toLinear(texture(uAlbedo, vUV).rgb * uColor);  // sRGB -> linear
    vec3 c = albedo * (hemiAmbient(N) + kSunColor * ndotl * sh);
    // Тинт к горизонту: uFogColor уже в ЛИНЕЙНОМ пространстве (микс до gamma), как в applyFog.
    float d = length(uViewPos.xz - vWorldPos.xz);
    float f = smoothstep(kFadeStart, kFadeEnd, d);
    c = mix(c, uFogColor, f);
    c = applyFog(c, vWorldPos);  // опциональный объёмный туман (обычно off) — совместимость
    fragColor = vec4(toGamma(c), 1.0);  // linear -> sRGB
}
