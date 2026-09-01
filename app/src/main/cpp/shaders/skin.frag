#version 450

// Vulkan skinning fragment shader: Lambert diffuse, hemispheric ambient, fog, gamma.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in vec4 vLightClip;

layout(set = 0, binding = 0) uniform Frame {
    mat4 uViewProj;
    mat4 uLightVP;
    vec4 uLightDir;
    vec4 uViewPos;
    vec4 uFogColor;
} frame;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;

layout(set = 1, binding = 0) uniform sampler2D uAlbedo;

layout(push_constant) uniform Push {
    mat4 uModel;
    vec4 uColor;
    int uBoneOffset;
} pc;

layout(location = 0) out vec4 fragColor;

vec3 toLinear(vec3 c) { return pow(c, vec3(2.2)); }
vec3 toGamma(vec3 c)  { return pow(c, vec3(1.0 / 2.2)); }
vec3 hemiAmbient(vec3 N) {
    const vec3 kSky = vec3(0.38, 0.44, 0.55);
    const vec3 kGround = vec3(0.30, 0.26, 0.22);
    return mix(kGround, kSky, N.y * 0.5 + 0.5) * 0.45;
}
vec3 applyFog(vec3 col, vec3 wp) {
    if (frame.uFogColor.w <= 0.0) return col;
    float d = length(frame.uViewPos.xyz - wp);
    float f = 1.0 - exp(-frame.uFogColor.w * d);
    return mix(col, frame.uFogColor.rgb, clamp(f, 0.0, 1.0));
}
const vec3 kSun = vec3(0.85);

float shadowFactor(vec4 lightClip, float ndotl) {
    vec3 p = lightClip.xyz / lightClip.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    if (p.z > 1.0 || p.z < 0.0) return 1.0;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    float b = frame.uLightDir.w;
    float bias = max(b * (1.0 - ndotl), b * 0.3);
    return texture(uShadowMap, vec3(uv, p.z - bias));
}

void main() {
    vec3 N = normalize(vNormal);
    float ndotl = max(dot(N, normalize(frame.uLightDir.xyz)), 0.0);
    float sh = shadowFactor(vLightClip, ndotl);
    vec3 albedo = toLinear(texture(uAlbedo, vUV).rgb * pc.uColor.rgb);
    vec3 c = albedo * (hemiAmbient(N) + kSun * ndotl * sh);
    c = applyFog(c, vWorldPos);
    fragColor = vec4(toGamma(c), 1.0);
}
