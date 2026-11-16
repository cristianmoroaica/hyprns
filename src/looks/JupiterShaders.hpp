#pragma once

// The GLSL of the look `jupiter` (looks/Jupiter.hpp), ported from the jupiter pair of
// reference/equivalence.html: JOVE_FORCE (the force), JOVE_DHEAD + JOVE_ADV_N(r16) and
// JOVE_MAC_N(r16) (the cloud, once per frame, R16F), JOVE_ADV / JOVE_MAC (the sketch's
// per-step cloud: PER_FRAME 0, R16 0) and JOVE_SHOW (the picture). Sketch coordinates
// (y up) everywhere but the picture pass, which flips once.
//
// Differences from the page, all forced by the plugin (listed in the task report):
// - the jets' count n is a uniform (7 on a lattice 225 high), the band profile's second
//   term 17 n / 7 (17 at n 7);
// - the lattice velocity is CFluid's RG32F attachment (.rg) instead of F2.yz;
// - texel indices wrap in x by a conditional +-width, never % on a negative int;
// - K is per axis (the cloud is round(4 x lattice x quality) each way);
// - the picture takes its pixel position from v_texcoord (the plugin's framebuffer may be
//   offset and rotated), the grain's cell from the monitor's pixels, the grain's frame from
//   a wall-clock tick (a 12-bit hash of it) instead of the frame count;
// - for stability, not forced: the force's Coriolis part is an exact rotation weighted by
//   rho instead of the page's f J dv - f^2 dv / 2 (see FORCE).

namespace JupiterShaders {

// JOVE_FORCE for CFluid (`size` is the lattice). The page's uniforms u0, relax, kick, beta
// are its constants here; `jets` is n.
inline constexpr const char* FORCE = R"GLSL(
uniform vec4 st[4];
uniform vec4 eye;
uniform float jets;
const float U0 = 0.04, RELAX = 1.5e-3, KICK = 6.5e-4, BETA = 4.5e-4;
vec2 force(ivec2 c, vec2 u, float rho) {
    vec2  p   = vec2(c) + 0.5;
    float y   = p.y / float(size.y);
    float jet = U0 * sin(3.14159265 * jets * y);
    vec2  F   = RELAX * vec2(jet - u.x, -0.3 * u.y);
    float f   = BETA * (p.y - 0.5 * float(size.y));
    vec2  dv  = u - vec2(jet, 0.0);
    // Deliberate deviation from the page (f J dv - f^2 dv / 2, a force density without rho):
    // CFluid moves u by F / rho per step, so that term rotates dv exactly only at rho 1 and
    // pumps energy where rho < 1 (storm cores near the walls of a tall lattice, where |f|
    // reaches 0.09). Here the Coriolis part turns dv by the angle -f exactly at any rho;
    // at rho 1 it is the page's term to O(f^3).
    float cf = cos(f), sf = sin(f);
    F += rho * (vec2(cf * dv.x + sf * dv.y, -sf * dv.x + cf * dv.y) - dv);
    for (int i = 0; i < 4; i++) {
        vec2  d = pdelta(p, st[i].xy);
        float r = st[i].z;
        if (r > 0.0)
            F += KICK * st[i].w * exp(-dot(d, d) / (2.0 * r * r)) * vec2(-d.y, d.x) / r;
    }
    if (eye.w > 0.0) {
        vec2  d  = pdelta(p, eye.xy);
        vec2  dd = vec2(d.x / 1.5, d.y);
        float r = eye.z, q2 = dot(dd, dd) / (r * r);
        vec2  vt = 0.07 * exp(0.5 - 0.5 * q2) * vec2(-dd.y, dd.x / 1.5) / r;
        F += 4e-3 * eye.w * exp(-q2 / 5.0) * (vt - u);
    }
    return F;
}
)GLSL";

// SROUND16 of the page: v (>= 0) written as one of the two half floats around it, the
// upper with probability its fraction of the way, so the store converts it exactly
// whatever the GPU's own float->half rounding is. h: a hash in [0, 1).
inline constexpr const char* SROUND16 = R"GLSL(
float hd(vec3 p) { p = fract(p * vec3(0.1031, 0.1030, 0.0973)); p += dot(p, p.yxz + 33.33); return fract((p.x + p.y) * p.z); }
// a half's ulp at v: 2^(e - 10), e the exponent of max(v, 2^-14) (a normal float), taken
// from its bits: (biased exponent - 10) << 23 is 2^(e - 10) as a float, exactly
float ulp16(float v) {
    float a = max(v, 6.103515625e-5);
    return uintBitsToFloat(((floatBitsToUint(a) >> 23) - 10u) << 23);
}
float sround16(float v, float h) { v = max(v, 0.0); float u = ulp16(v); float lo = floor(v / u) * u; return lo + (h < (v - lo) / u ? u : 0.0); }
)GLSL";

// JOVE_DHEAD: the cloud's size, the lattice's, the lattice velocity by hand-bilinear
// (four texelFetch, x wrapped, y clamped), K per axis.
inline constexpr const char* CLOUD_HEAD = R"GLSL(
uniform sampler2D S;
uniform sampler2D V;
uniform ivec2 size;
uniform ivec2 lsize;
uniform vec2 K;
layout(location = 0) out vec4 o;
// one wrap of x into [0, n): every index here is at most one width outside
int wrapI(int x, int n) { if (x < 0) x += n; else if (x >= n) x -= n; return x; }
vec2 lv(ivec2 b) { return texelFetch(V, ivec2(wrapI(b.x, lsize.x), clamp(b.y, 0, lsize.y - 1)), 0).rg; }
vec2 velAt(vec2 pl) {
    vec2  g = pl - 0.5;
    vec2  i = floor(g), f = g - i;
    ivec2 b = ivec2(i);
    return mix(mix(lv(b), lv(b + ivec2(1, 0)), f.x), mix(lv(b + ivec2(0, 1)), lv(b + ivec2(1, 1)), f.x), f.y);
}
float band(float y, float jets, float fine) { return 0.5 + 0.36 * sin(3.14159265 * jets * y + 1.5708) + 0.14 * sin(3.14159265 * fine * y + 2.0); }
)GLSL";

// JOVE_ADV_N(r16) (PER_FRAME 1: dir = +-n) / JOVE_ADV (PER_FRAME 0: dir = +-1, R16 0):
// one semi-Lagrangian trace with the hardware's linear filter.
inline constexpr const char* ADVECT = R"GLSL(
uniform float dir;
uniform float frame;
void main() {
    vec2  c = gl_FragCoord.xy;
    float v = texture(S, (c - dir * K * velAt(c / K)) / vec2(size)).r;
#if R16
    v = sround16(v, hd(vec3(c + (dir > 0.0 ? 13.1 : 29.7), frame)));
#endif
    o = vec4(v, 0.0, 0.0, 1.0);
}
)GLSL";

// JOVE_MAC_N(r16) / JOVE_MAC: MacCormack, clamped to the four cloud cells around the
// departure point, then the sources: the bands (back), the storms (toward their colour),
// the eye. Per frame every factor a becomes comp(a) = 1 - (1 - a)^nst (backN from C++).
inline constexpr const char* MACCORMACK = R"GLSL(
uniform sampler2D HAT;
uniform sampler2D BAR;
uniform vec4 st[4];
uniform vec4 sc;
uniform float backN;
uniform vec4 eye;
uniform float nst;
uniform float frame;
uniform float jets;
uniform float fine;
float sm(ivec2 p) { return texelFetch(S, ivec2(wrapI(p.x, size.x), clamp(p.y, 0, size.y - 1)), 0).r; }
#if PER_FRAME
float comp(float a) { return 1.0 - pow(max(1.0 - a, 0.0), nst); }
#else
float comp(float a) { return a; }
#endif
void main() {
    ivec2 c  = ivec2(gl_FragCoord.xy);
    vec2  cf = gl_FragCoord.xy;
    vec2  p  = cf / K;
    ivec2 b  = ivec2(floor(cf - 0.5 - K * nst * velAt(p)));
    float a0 = sm(b), a1 = sm(b + ivec2(1, 0)), a2 = sm(b + ivec2(0, 1)), a3 = sm(b + ivec2(1, 1));
    float v  = texelFetch(HAT, c, 0).r + 0.5 * (texelFetch(S, c, 0).r - texelFetch(BAR, c, 0).r);
    v        = clamp(v, min(min(a0, a1), min(a2, a3)), max(max(a0, a1), max(a2, a3)));
    v        = mix(v, band(p.y / float(lsize.y), jets, fine), backN);
    float sx = float(lsize.x);
    for (int i = 0; i < 4; i++) {
        vec2 d = p - st[i].xy;
        d.x -= sx * floor(d.x / sx + 0.5);
        float r = st[i].z;
        if (r > 0.0)
            v = mix(v, sc[i], comp(0.004 * abs(st[i].w) * exp(-dot(d, d) / (0.5 * r * r))));
    }
    if (eye.w > 0.0) {
        vec2 d = p - eye.xy;
        d.x -= sx * floor(d.x / sx + 0.5);
        d.x /= 1.5;
        float q2 = dot(d, d) / (eye.z * eye.z);
        float qq = sqrt(q2);
        float tg = 0.74 + 0.26 * exp(-q2 * 8.0) - 0.5 * exp(-(qq - 1.3) * (qq - 1.3) * 12.0);
        v        = mix(v, tg, comp(0.008 * eye.w * exp(-q2 * 0.45)));
    }
    v = clamp(v, 0.0, 1.0);
#if R16
    v = sround16(v, hd(vec3(cf, frame)));
#endif
    o = vec4(v, 0.0, 0.0, 1.0);
}
)GLSL";

// sketchJove's seed(): band((y + 0.5) / DH) on every row.
inline constexpr const char* SEED = R"GLSL(
uniform float jets;
uniform float fine;
void main() {
    float v = band(gl_FragCoord.y / float(size.y), jets, fine);
#if R16
    v = sround16(v, hd(vec3(gl_FragCoord.xy, 4099.0)));
#endif
    o = vec4(v, 0.0, 0.0, 1.0);
}
)GLSL";

// A quality change: the old cloud (S, linear, x repeating, y clamped) at the new size.
inline constexpr const char* RESAMPLE = R"GLSL(
void main() {
    float v = texture(S, gl_FragCoord.xy / vec2(size)).r;
#if R16
    v = sround16(v, hd(vec3(gl_FragCoord.xy, 4111.0)));
#endif
    o = vec4(v, 0.0, 0.0, 1.0);
}
)GLSL";

// JOVE_SHOW: the six-step ramp with a relief, colour fringing toward the edges, the
// contrast curve, a cool tint in the shadows, the vignette and the sensor grain. q is the
// sketch's gl_FragCoord / view (y up); `view` the picture, `mon` the monitor (pixels),
// `grain` the 12-bit frame value of the wall-clock tick (the sketch's floor(tick / 2.5)).
inline constexpr const char* PICTURE = R"GLSL(
uniform sampler2D S;
uniform ivec2 size;
uniform vec2 view;
uniform vec2 mon;
uniform float grain;
uniform float fade;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;
const vec3 R0 = vec3(0.070, 0.095, 0.135), R1 = vec3(0.030, 0.030, 0.040), R2 = vec3(0.135, 0.085, 0.065), R3 = vec3(0.270, 0.170, 0.105), R4 = vec3(0.520, 0.365, 0.160), R5 = vec3(0.045, 0.040, 0.045);
vec3 ramp(float t) {
    t = clamp(t, 0.0, 1.0) * 5.0;
    int   i = int(floor(t));
    float f = fract(t);
    f = f * f * (3.0 - 2.0 * f);
    if (i == 0) return mix(R0, R1, f);
    if (i == 1) return mix(R1, R2, f);
    if (i == 2) return mix(R2, R3, f);
    if (i == 3) return mix(R3, R4, f);
    if (i == 4) return mix(R4, R5, f);
    return R5;
}
float h3(vec3 p) { p = fract(p * vec3(0.1031, 0.1030, 0.0973)); p += dot(p, p.yxz + 33.33); return fract((p.x + p.y) * p.z); }
vec3 shade(vec2 q) {
    vec2  px = 2.0 / vec2(size);
    float c  = texture(S, q).r;
    float cx = texture(S, q + vec2(px.x, 0.0)).r - texture(S, q - vec2(px.x, 0.0)).r, cy = texture(S, q + vec2(0.0, px.y)).r - texture(S, q - vec2(0.0, px.y)).r;
    return ramp(c) * (1.0 + clamp(1.5 * dot(vec2(cx, cy), vec2(-0.6, 0.6)), -0.08, 0.08));
}
void main() {
    vec2  q  = vec2(v_texcoord.x, 1.0 - v_texcoord.y); // the one flip: y up, as the sketch's canvas
    vec2  e  = (q - 0.5) * vec2(view.x / view.y, 1.0);
    float rr = dot(e, e);
    vec2  ca = (q - 0.5) * 0.0035 * rr;
    vec3  col = vec3(shade(q + ca).r, shade(q).g, shade(q - ca).b);
    col = col * col * (3.0 - 2.0 * col) * 0.35 + col * 0.75;
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col += vec3(0.000, 0.006, 0.012) * (1.0 - smoothstep(0.0, 0.25, lum));
    col *= mix(1.0, 0.30, smoothstep(0.15, 1.05, rr));
    float fr = grain;
    // the grain cell from the integer monitor pixel (cells of 1.5 pixels: 2 cells per 3
    // pixels), so a pixel centre never sits on a cell boundary under rotation
    ivec2 ip = ivec2(floor(q * mon));
    vec2  gp = vec2((ip * 2) / 3);
    float n1 = h3(vec3(gp, fr)) + h3(vec3(gp + 17.0, fr + 3.0)) - 1.0;
    vec3  nc = vec3(h3(vec3(gp + 31.0, fr)), h3(vec3(gp + 47.0, fr)), h3(vec3(gp + 59.0, fr))) - 0.5;
    col += (0.030 + 0.050 * sqrt(max(lum, 0.0))) * (n1 * 0.8 + nc * 0.35);
    col = max(col, 0.0);
    fragColor = vec4(col * fade, 1.0);
}
)GLSL";

} // namespace JupiterShaders
