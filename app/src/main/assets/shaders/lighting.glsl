// Common lighting helpers: gamma, hemispheric ambient.
// Requires common.glsl included ABOVE this (uses uLightDir etc. from Frame).
// ASCII only (some GL drivers reject non-ASCII even in comments).

// sRGB <-> linear. We light in linear space, then convert back on output,
// otherwise the diffuse ramp looks muddy (dark mids). Approx gamma 2.2.
vec3 toLinear(vec3 c) { return pow(c, vec3(2.2)); }
vec3 toGamma(vec3 c)  { return pow(c, vec3(1.0 / 2.2)); }

// Hemispheric ambient: cool sky light from above, warm bounce from the ground
// below, blended by the surface normal's up component. Replaces a flat ambient
// term so shaded sides keep some tinted fill instead of going dead-flat.
vec3 hemiAmbient(vec3 N) {
    const vec3 kSky    = vec3(0.38, 0.44, 0.55);  // cool top
    const vec3 kGround = vec3(0.30, 0.26, 0.22);  // warm bottom
    return mix(kGround, kSky, N.y * 0.5 + 0.5) * 0.45;
}

// Directional (sun) intensity. Kept below 1 so ambient+sun rarely blows out.
const vec3 kSunColor = vec3(0.85);

// Shadow map (directional light). Hardware PCF via sampler2DShadow: LINEAR filter
// + COMPARE_REF_TO_TEXTURE gives a free 2x2 soft edge on mobile tilers.
// highp: depth math needs full precision on GLES, else acne/swimming.
uniform highp sampler2DShadow uShadowMap;

// Exponential distance fog, blended in LINEAR space (before gamma). Fades distant
// geometry into uFogColor -> depth cue + hides the far arena edge. Density 0 = off.
vec3 applyFog(vec3 colorLinear, vec3 worldPos) {
    if (uFogDensity <= 0.0) return colorLinear;
    float dist = length(uViewPos - worldPos);
    float f = 1.0 - exp(-uFogDensity * dist);
    return mix(colorLinear, uFogColor, clamp(f, 0.0, 1.0));
}

// Lit fraction in [0,1] (0 = fully shadowed, 1 = fully lit). ndotl steers the
// slope-scaled bias. Fragments outside the map or past its far plane -> lit.
float shadowFactor(highp vec4 lightClip, float ndotl) {
    highp vec3 p = lightClip.xyz / lightClip.w;   // perspective divide (ortho: w=1)
    p = p * 0.5 + 0.5;                            // NDC [-1,1] -> [0,1]
    if (p.z > 1.0) return 1.0;                    // beyond the shadow far plane
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 1.0;  // outside map
    highp float bias = max(uShadowBias * (1.0 - ndotl), uShadowBias * 0.3);
    return texture(uShadowMap, vec3(p.xy, p.z - bias));  // 0..1, HW PCF
}
