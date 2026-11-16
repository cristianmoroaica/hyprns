#pragma once

// GLSL of the look `marble` (Marble.hpp): the stirrers' force, the displacement's post,
// the stone, the bake pass and the picture. Ported from sketchMarble(X, true, "7200.0",
// "1.4", "1.6", "0.33") in reference/a2n-sketch.html (the second card, A2n; numbers in
// reference/2026-11-15-look-A2n-chosen.md), with the accepted speed-ups of
// reference/equivalence.html (MARBLE_BAKE, MARBLE_PIC, MARBLE_POST_N). No pores (the
// card's PO is 0: its picture is c0, the stone without pores).
//
// Orientation: as in every ported look (PatternShaders.hpp), the lattice, the
// displacement, the stone's coordinates and the bake are in the sketch's own
// coordinates, y up (row 0 at the bottom); the picture pass flips once
// (t = (v_texcoord.x, 1 - v_texcoord.y)).
//
// Units: the stone is a function of m, a position in units of the picture's short side
// (the sketch's p = q * (view.x / view.y, 1) on a landscape canvas). The displacement d is
// in lattice cells; the stone is sampled at m = p - d * dScale, dScale = (view / S) /
// lattice per axis (the sketch's d / 225 on its 400x225 lattice).

namespace MarbleShaders {

// The sketch's force (CFluid force snippet; pdelta from Fields.hpp): three stirrers,
// stir[i] = (x, y, vx, vy) in cells and cells per fluid step, env[i] their unevenness.
inline constexpr const char* FORCE = R"GLSL(
uniform vec4 stir[3];
uniform vec3 env;
vec2 force(ivec2 c, vec2 u, float rho) {
    vec2 p = vec2(c) + 0.5;
    vec2 F = -6e-4 * u;
    for (int i = 0; i < 3; i++) {
        vec2 d = pdelta(p, stir[i].xy);
        F += 0.05 * env[i] * exp(-dot(d, d) / 256.0) * (stir[i].zw - u);
    }
    return F;
}
)GLSL";

// The displacement's post (a CField RGBA32F, periodic): n fluid steps of the sketch's
// (d + u) * relax in one go, d r^n + u r (1 - r^n) / (1 - r) (relaxN = r^n, gsum the sum;
// one step: both r). .z keeps |u| of the last fluid step for the health check.
inline constexpr const char* POST = R"GLSL(
uniform float relaxN;
uniform float gsum;
vec4 post(ivec2 c, vec4 v) {
    vec2 u = texelFetch(fieldVel, c, 0).rg;
    return vec4(v.xy * relaxN + u * gsum, length(u), 1.0);
}
)GLSL";

// The stone at m: colour and height h0 (the sketch's DARK branch before lighting).
// Time enters through uniforms computed in C++ in double (CMarbleLook::stoneAt): the four
// vein families' life and seed (sd) and the brass epochs' offsets and blend. The value
// noise hashes its lattice corners as integers (as pattern's), offset per monitor by
// hashBase: the sketch's float hash fh loses its digits as the seeds grow.
// Exact skips: a vein family whose life or area mask is 0 adds exactly 0 in the sketch,
// so its ridge fbm is not computed.
inline constexpr const char* STONE = R"GLSL(
uniform vec4 veinLife;
uniform vec4 veinSeed;
uniform vec3 brassSeed;
uniform ivec2 hashBase;

float hashCell(ivec2 i) {
    i += hashBase;
    uint h = uint(i.x) * 0x8da6b343u ^ uint(i.y) * 0xd8163841u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h >> 8) * (1.0 / 16777216.0);
}
float fn(vec2 p) {
    vec2  fl = floor(p);
    vec2  f  = p - fl;
    ivec2 i  = ivec2(fl);
    f        = f * f * (3.0 - 2.0 * f);
    return mix(mix(hashCell(i), hashCell(i + ivec2(1, 0)), f.x), mix(hashCell(i + ivec2(0, 1)), hashCell(i + ivec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < 5; i++) {
        s += a * fn(p);
        p = p * 2.03 + 7.1;
        a *= 0.5;
    }
    return s / 0.96875;
}

const float SC[4] = float[4](1.5, 3.6, 2.4, 6.0);
const float WT[4] = float[4](1.0, 0.6, 0.8, 0.4);

vec4 stone(vec2 m) {
    vec2  ma = mat2(0.80, 0.60, -0.60, 0.80) * m * vec2(1.0, 2.6);
    vec2  wv = vec2(fbm(m * 1.3 + 2.0), fbm(m * 1.3 + 9.0));
    float cl = fbm(ma * 1.1 + wv * 1.4);
    float vein = 0.0, haze = 0.0;
    for (int k = 0; k < 4; k++) {
        float sd = veinSeed[k];
        float w  = WT[k] * veinLife[k];
        if (w > 0.0)
            w *= smoothstep(0.50, 0.72, fn(m * 1.7 + sd * 0.7 + 20.0));
        if (w > 0.0) {
            float r  = fbm(ma * SC[k] + wv * 2.4 + sd);
            float rd = 1.0 - abs(2.0 * r - 1.0);
            vein += w * pow(rd, 28.0);
            haze += w * pow(rd, 5.0);
        }
    }
    vein = clamp(vein + 0.16 * haze, 0.0, 1.0);
    vec3 col = mix(vec3(0.060, 0.058, 0.070), vec3(0.115, 0.110, 0.128), smoothstep(0.25, 0.85, cl));
    col      = mix(col, vec3(0.80, 0.79, 0.76), vein * 0.6);
    float bz = fbm(m * 2.2 + 70.0 + brassSeed.x);
    if (brassSeed.z > 0.0)
        bz = mix(bz, fbm(m * 2.2 + 70.0 + brassSeed.y), brassSeed.z);
    col += mix(vec3(0.55, 0.38, 0.20), vec3(0.80, 0.64, 0.36), fn(m * 3.0 + 90.0)) * 0.075 * smoothstep(0.50, 0.72, bz);
    float h0 = 0.7 * fbm(m * 5.0 + 3.0) + 0.05 * fbm(m * 60.0) - 0.10 * vein;
    return vec4(col, h0);
}
)GLSL";

// One strip of a key (CBake): the stone at every texel of the strip's rows, colour and
// height (h0 in -0.1..0.75 stored as (h0 + 0.1) / 0.85, as MARBLE_BAKE). The bake covers
// the picture plus a margin on every side (the displacement reaches past the edges):
// texel centre gl_FragCoord.xy is m = (gl_FragCoord.xy - margin) / S. gl_FragCoord is in
// window coordinates, so the strip's viewport offset is already in it.
inline constexpr const char* BAKE = R"GLSL(
uniform float invShort;
uniform float margin;
layout(location = 0) out vec4 o;
void main() {
    vec4 s = stone((gl_FragCoord.xy - margin) * invShort);
    o      = vec4(s.rgb, clamp((s.a + 0.1) / 0.85, 0.0, 1.0));
}
)GLSL";

// The picture (MARBLE_PIC): the displacement at the pixel by the B-spline (16 taps),
// then the stone at m: one tap of each shown key, blended (BAKED), or the stone itself
// (the sketch's per-pixel path, bench only). Lighting and sheen from the height's
// screen derivatives, as the sketch's dFdx/dFdy: brought back to the picture's own axes
// (y up) through the derivatives of the pixel position, so a rotated output lights the
// same side; gh0 = dh0/dp / 150, p in short sides. The dither (+-0.5/255) is added here,
// after the 8-bit bake. fade scales the picture to black.
inline constexpr const char* PICTURE = R"GLSL(
uniform sampler2D disp;
#if BAKED
uniform sampler2D keyA;
uniform sampler2D keyB;
uniform float blend;
uniform vec4 bakeMap;
#endif
uniform vec2 view;
uniform vec2 dScale;
uniform vec3 light;
uniform float fade;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

float fh(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}

void main() {
    vec2  t   = vec2(v_texcoord.x, 1.0 - v_texcoord.y); // the one flip: y up
    vec2  pix = t * view;
    float S   = min(view.x, view.y);
    vec2  p   = pix / S;
    vec2  d   = bilinear(disp, t * vec2(textureSize(disp, 0)), true).xy; // the sketch's MARBLE_PIC: bilinear
    vec2  m   = p - d * dScale;
#if BAKED
    vec2 bu = m * bakeMap.xy + bakeMap.zw;
    vec4 st = mix(texture(keyA, bu), texture(keyB, bu), blend);
    st.a    = st.a * 0.85 - 0.1;
#else
    vec4 st = stone(m);
#endif
    float h0 = st.a;
    vec2  jx = dFdx(pix), jy = dFdy(pix);
    float hx = dFdx(h0), hy = dFdy(h0);
    vec2  g  = vec2(hx * jy.y - hy * jx.y, jx.x * hy - jy.x * hx) / (jx.x * jy.y - jy.x * jx.y);
    vec2  gh0  = g * (S / 150.0);
    float lit0 = clamp(1.0 - dot(gh0, vec2(-0.5, 0.5)), 0.6, 1.3);
    vec3  Ld   = normalize(light - vec3(p, 0.0));
    vec3  hv   = normalize(Ld + vec3(0.0, 0.0, 1.0));
    float sp0  = pow(max(dot(normalize(vec3(-gh0 * 1.2, 1.0)), hv), 0.0), 24.0);
    vec3  col  = st.rgb * mix(1.0, lit0, 0.9) + 0.33 * vec3(0.20, 0.20, 0.22) * sp0;
    col += (fh(gl_FragCoord.xy * 0.731) - 0.5) / 255.0;
    fragColor = vec4(col * fade, 1.0);
}
)GLSL";

} // namespace MarbleShaders
