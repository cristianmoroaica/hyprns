#pragma once

// GLSL of the look `embers` (Embers.hpp): the fluid's force, the heat's post (per frame,
// and per step for the sketch's path), the proximity blur, the coal layer, its bake pass
// and the picture. Ported from the embers pair of reference/equivalence.html (FN_C4Q,
// FIRE_FORCE, FIRE_POST_N, FIRE_POST, FIRE_BLUR, COAL_FN, COAL_BAKE, firePicSrc with the
// CORAL palette, TEX 3, from above, reach 14, strength 0.35, creep 6.5e-4); numbers in
// reference/2026-11-15-look-C4q-locked.md.
//
// Orientation: as in every ported look, the lattice, the heat, the blur, the coal layer
// and its bake are in the sketch's own coordinates, y up (row 0 at the bottom); the
// picture pass flips once (t = (v_texcoord.x, 1 - v_texcoord.y)).
//
// Noise: the sketch hashes noise corners with the float hash fh, whose digits go as its
// arguments grow. The heat bed and the flicker keep the sketch's fh: their noise is
// periodic over the lattice (a whole number of noise cells per axis,
// CEmbersLook::bedPeriods; the sketch's own fnp for the bed), so the corners are wrapped
// to their period and stay small integers, and the drift is reduced modulo the period in
// C++ in double: exact however long the look runs, and on the sketch's 400x225 lattice the
// sketch's own bed. The coal layer's noise is not periodic and its creep grows without
// bound: it hashes integer corners (hashCell, as marble's stone), offset per monitor by
// hashBase, each creeping octave's offset passed as an integer and a fraction; the coals'
// turning and the pulse use an angle reduced modulo 2 pi with a quantised rate (below).

namespace EmbersShaders {

// The sketch's force (FIRE_FORCE with gb 0: from above, no buoyancy): wind and gust,
// drag 1.2e-3, three stirrers (stir[i] = (x, y, vx, vy) in cells and cells per step,
// env[i] their unevenness; force radius 16 cells).
inline constexpr const char* FORCE = R"GLSL(
uniform vec4 stir[3];
uniform vec3 env;
uniform vec2 wind;
vec2 force(ivec2 c, vec2 u, float rho) {
    vec2 p = vec2(c) + 0.5;
    vec2 F = wind - 1.2e-3 * u;
    for (int i = 0; i < 3; i++) {
        vec2 d = pdelta(p, stir[i].xy);
        F += 0.05 * env[i] * exp(-dot(d, d) / 256.0) * (stir[i].zw - u);
    }
    return F;
}
)GLSL";

// The integer hash of a noise corner (marble's): uint arithmetic, which wraps.
inline constexpr const char* HASH = R"GLSL(
uniform ivec2 hashBase;
float hashU(uvec2 u) {
    u += uvec2(hashBase);
    uint h = u.x * 0x8da6b343u ^ u.y * 0xd8163841u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h >> 8) * (1.0 / 16777216.0);
}
float hashCell(ivec2 i) {
    return hashU(uvec2(i));
}
)GLSL";

// The heat bed (the sketch's bedv) and the flicker as periodic value noise (the sketch's
// fnp): per noise cells per period on each axis, the corners wrapped once per axis
// (wrapIndex, nextIndex: GLSL_BILINEAR) and hashed by the sketch's fh. x in noise units.
// bedv(uv, off): uv in 0..1 over the lattice, off = (layer 1's offset, added; layer 2's,
// subtracted), as the sketch's
//   0.6 fnp(uv (10, 6) + (t 3e-5, t 1.1e-5)) + 0.4 fnp(uv (16, 9) - (t 2e-5, -t 2.6e-5)), t = step x shift.
// The drifts per step (bedDrift, flickDir x 4e-3) and the periods are the lattice's
// (swapped on a portrait lattice); set once per build.
inline constexpr const char* BED = R"GLSL(
uniform ivec4 bedPer;   // layer 1 (x, y), layer 2 (x, y)
uniform ivec2 flickPer;
uniform vec4 bedDrift;  // per step: layer 1's (added), layer 2's (subtracted)
uniform vec2 flickDir;  // the flicker's drift axis (the sketch's y)
float fh(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}
float fnp(vec2 x, ivec2 per) {
    vec2 fl = floor(x);
    vec2 f  = x - fl;
    f       = f * f * (3.0 - 2.0 * f);
    ivec2 i = ivec2(fl);
    int x0 = wrapIndex(i.x, per.x), y0 = wrapIndex(i.y, per.y);
    float a0 = float(x0), b0 = float(y0), a1 = float(nextIndex(x0, per.x)), b1 = float(nextIndex(y0, per.y));
    return mix(mix(fh(vec2(a0, b0)), fh(vec2(a1, b0)), f.x), mix(fh(vec2(a0, b1)), fh(vec2(a1, b1)), f.x), f.y);
}
float bedv(vec2 uv, vec4 off) {
    return 0.6 * fnp(uv * vec2(bedPer.xy) + off.xy, bedPer.xy) + 0.4 * fnp(uv * vec2(bedPer.zw) - off.zw, bedPer.zw);
}
const float C1 = 4e-4, C2 = 3e-4, LO = 0.66, HI = 0.76;
)GLSL";

// The flicker, in the heat's posts (size: the field's): the sketch's fn(c 0.08 + (0, t 4e-3)),
// c the cell's corner, periodic over the lattice.
inline constexpr const char* FLICKER = R"GLSL(
float flicker(ivec2 c, float off) {
    return fnp(vec2(c) * vec2(flickPer) / vec2(size) + flickDir * off, flickPer);
}
// fnp over a frame without a hash per step: the hashes of the 3 x 3 corners from the lower
// of the cells of xa and xb (the first and the last step's x), wrapped as fnp wraps them.
// A frame's drift is under one noise cell on every axis (at most 64 steps: 0.26 for the
// flicker, 0.02 for the bed), so every x between them sits in one of the 2 x 2 cells these
// corners span, and fnpC(x, corners) is fnp(x, per) with the same hash values.
struct SCorners {
    vec3  r0, r1, r2; // rows y0, y1, y2; columns x0, x1, x2
    ivec2 i0;
};
SCorners corners(vec2 xa, vec2 xb, ivec2 per) {
    SCorners n;
    n.i0   = ivec2(min(floor(xa), floor(xb)));
    int  x0 = wrapIndex(n.i0.x, per.x), x1 = nextIndex(x0, per.x), x2 = nextIndex(x1, per.x);
    int  y0 = wrapIndex(n.i0.y, per.y), y1 = nextIndex(y0, per.y), y2 = nextIndex(y1, per.y);
    vec3 a  = vec3(float(x0), float(x1), float(x2));
    n.r0    = vec3(fh(vec2(a.x, float(y0))), fh(vec2(a.y, float(y0))), fh(vec2(a.z, float(y0))));
    n.r1    = vec3(fh(vec2(a.x, float(y1))), fh(vec2(a.y, float(y1))), fh(vec2(a.z, float(y1))));
    n.r2    = vec3(fh(vec2(a.x, float(y2))), fh(vec2(a.y, float(y2))), fh(vec2(a.z, float(y2))));
    return n;
}
float fnpC(vec2 x, SCorners n) {
    vec2 fl = floor(x);
    vec2 f  = x - fl;
    f       = f * f * (3.0 - 2.0 * f);
    ivec2 s = ivec2(fl) - n.i0; // 0 or 1 on each axis
    vec3  lo = s.y == 0 ? n.r0 : n.r1, hi = s.y == 0 ? n.r1 : n.r2;
    vec2  l = s.x == 0 ? lo.xy : lo.yz, h = s.x == 0 ? hi.xy : hi.yz;
    return mix(mix(l.x, l.y, f.x), mix(h.x, h.y, f.x), f.y);
}
)GLSL";

// The heat's post once per frame (FIRE_POST_N; CField RG32F, periodic, fieldDt = n, the
// frame's steps, at most 64). The transported value is mixed toward the four-neighbour
// mean of the old field taken at the departure point (where it came from; four bilinear
// taps a cell apart), aN = 1 - (1 - 4 kappa)^n: always bounded, as the fixed page. This
// diffusion saturates near one cell per frame: its spread is 76% of n per-step diffusions
// at n = 8 and 26% at n = 48 (accepted with embers-heat-per-frame). Decay compounded
// exactly (rN, c2N). The sources are max() floors set at each step t = tstart + k and then
// decayed over the n - 1 - k steps left; the loop is capped at 64 steps (the plugin's
// clock never gives a frame more; the look cuts a longer one). The pop holds 1.7 while it
// lives: its floor is the one set at its last alive step of the frame, popM steps before
// the end. .g keeps |u| of the last step for the health check.
inline constexpr const char* POST_FRAME = R"GLSL(
uniform float aN;
uniform float rN;
uniform float c2N;
uniform vec4 pop;     // x, y, radius (0: none this frame), heat
uniform float popM;
uniform vec4 bed;     // the bed's offsets at tstart
uniform float flick;  // the flicker's offset at tstart
// decay over m = 0..63 steps, from C++ in double (set in CEmbersLook::build): r = (1 - c1)^m
// and c2 (1 - r) / c1, so no pow per step of the loop
uniform float decayR[64];
uniform float decayC[64];
float decayN(float f, int m) {
    return max(f * decayR[m] - decayC[m], 0.0);
}
vec4 post(ivec2 c, vec4 s) {
    float nst = fieldDt;
    vec2  dp  = vec2(c) + 0.5 - displacement(c);
    float avg = 0.25 * (bilinear(fieldSrc, dp + vec2(-1.0, 0.0), WRAPY).r + bilinear(fieldSrc, dp + vec2(1.0, 0.0), WRAPY).r +
                        bilinear(fieldSrc, dp + vec2(0.0, -1.0), WRAPY).r + bilinear(fieldSrc, dp + vec2(0.0, 1.0), WRAPY).r);
    float T   = max(mix(s.r, avg, aN) * rN - c2N, 0.0);
    vec2  uv  = (vec2(c) + 0.5) / vec2(size);
    float fl  = 0.0;
    if (nst >= 3.0) {
        // the same values with the corners' hashes taken once per frame (corners, fnpC);
        // the expressions for x are the loop's below, term for term
        vec2     fx = vec2(c) * vec2(flickPer) / vec2(size);
        vec2     b1 = uv * vec2(bedPer.xy), b2 = uv * vec2(bedPer.zw);
        float    kl = nst - 1.0;
        vec4     ol = bed + kl * bedDrift;
        SCorners c1 = corners(b1 + bed.xy, b1 + ol.xy, bedPer.xy);
        SCorners c2 = corners(b2 - bed.zw, b2 - ol.zw, bedPer.zw);
        SCorners cf = corners(fx + flickDir * flick, fx + flickDir * (flick + kl * 4e-3), flickPer);
        for (int k = 0; k < 64; k++) {
            if (float(k) >= nst)
                break;
            vec4  off  = bed + float(k) * bedDrift;
            float heat = smoothstep(LO, HI, 0.6 * fnpC(b1 + off.xy, c1) + 0.4 * fnpC(b2 - off.zw, c2));
            float fk   = fnpC(fx + flickDir * (flick + float(k) * 4e-3), cf);
            fl         = max(fl, decayN(heat * (0.35 + 0.85 * fk), int(nst) - 1 - k));
        }
    } else
        for (int k = 0; k < 64; k++) {
            if (float(k) >= nst)
                break;
            float heat = smoothstep(LO, HI, bedv(uv, bed + float(k) * bedDrift));
            float fk   = flicker(c, flick + float(k) * 4e-3);
            fl         = max(fl, decayN(heat * (0.35 + 0.85 * fk), int(nst) - 1 - k));
        }
    T = max(T, fl);
    if (pop.z > 0.0)
        T = max(T, decayN(pop.w * smoothstep(pop.z, pop.z - 3.0, length(pdelta(vec2(c) + 0.5, pop.xy))), int(popM)));
    return vec4(T, length(texelFetch(fieldVel, c, 0).rg), 0.0, 1.0);
}
)GLSL";

// The sketch's own heat step (FIRE_POST), per step: diffusion by the old field's
// Laplacian at the cell, decay, the bed and flicker floor at this step, the pop.
inline constexpr const char* POST_STEP = R"GLSL(
uniform vec4 pop;
uniform vec4 bed;
uniform float flick;
vec4 post(ivec2 c, vec4 s) {
    int   xl  = c.x == 0 ? size.x - 1 : c.x - 1, xr = nextIndex(c.x, size.x);
    int   yd  = c.y == 0 ? size.y - 1 : c.y - 1, yu = nextIndex(c.y, size.y);
    float lap = texelFetch(fieldSrc, ivec2(xl, c.y), 0).r + texelFetch(fieldSrc, ivec2(xr, c.y), 0).r + texelFetch(fieldSrc, ivec2(c.x, yd), 0).r +
                texelFetch(fieldSrc, ivec2(c.x, yu), 0).r - 4.0 * texelFetch(fieldSrc, c, 0).r;
    float T    = max((s.r + 0.02 * lap) * (1.0 - C1) - C2, 0.0);
    float heat = smoothstep(LO, HI, bedv((vec2(c) + 0.5) / vec2(size), bed));
    T          = max(T, heat * (0.35 + 0.85 * flicker(c, flick)));
    if (pop.z > 0.0)
        T = max(T, pop.w * smoothstep(pop.z, pop.z - 3.0, length(pdelta(vec2(c) + 0.5, pop.xy))));
    return vec4(T, length(texelFetch(fieldVel, c, 0).rg), 0.0, 1.0);
}
)GLSL";

// One axis of the proximity blur (FIRE_BLUR): taps -14..14 cells along dir, periodic, the
// Gaussian's weights (sigma from the sketch's 32-tap ring, ringSigma(14)) normalised in C++.
inline constexpr const char* BLUR = R"GLSL(
uniform sampler2D src;
uniform ivec2 dir;
uniform float wk[15]; // |i| = 0..14
layout(location = 0) out vec4 o;
void main() {
    ivec2 c  = ivec2(gl_FragCoord.xy);
    ivec2 sz = textureSize(src, 0);
    float a  = 0.0;
    for (int i = -14; i <= 14; i++) {
        ivec2 p = c + dir * i;
        p.x     = p.x < 0 ? p.x + sz.x : p.x >= sz.x ? p.x - sz.x : p.x;
        p.y     = p.y < 0 ? p.y + sz.y : p.y >= sz.y ? p.y - sz.y : p.y;
        a += wk[i < 0 ? -i : i] * texelFetch(src, p, 0).r;
    }
    o = vec4(a, 0.0, 0.0, 1.0);
}
)GLSL";

// The coal layer (COAL_FN and the coal part of COAL_BAKE) at p, in units of the picture's
// short side (24 coals per short side): a Voronoi of jittered, slowly turning points,
// warped by a coarse noise and a fine creeping fbm; edge = distance to the nearest
// boundary, id = the nearest coal's tint, brk = where its cracks break up, wd = the crack
// width (fwidth of the edge: per pixel of the target it is drawn at).
// Time (CEmbersLook::setCoal, from the step in double, tt = step x 6.5e-4): the creeping
// fbms' offsets (+tt 0.3, -tt 0.3, +tt 0.5, the same on both axes) per octave as an integer
// (creepI) and a fraction (creepF), the octave's point taken as floor(x + f) + i; the
// coals' turning tt (0.5 + cid) with cid quantised to 1/256: (128 + k) theta, theta =
// tt / 256 mod 2 pi (a turning rate off by less than 1/512).
inline constexpr const char* COAL = R"GLSL(
uniform ivec4 creepI[3];
uniform vec4 creepF[3];
uniform float theta;
float fnI(vec2 x, int oi) {
    vec2 fl = floor(x);
    vec2 f  = x - fl;
    f       = f * f * (3.0 - 2.0 * f);
    uvec2 i = uvec2(ivec2(fl)) + uvec2(uint(oi));
    return mix(mix(hashU(i), hashU(i + uvec2(1u, 0u)), f.x), mix(hashU(i + uvec2(0u, 1u)), hashU(i + uvec2(1u, 1u)), f.x), f.y);
}
float fbmT(vec2 p, ivec4 oi, vec4 of) {
    float a = 0.5, s = 0.0;
    for (int k = 0; k < 4; k++) {
        s += a * fnI(p + of[k], oi[k]);
        p = p * 2.03 + 7.1;
        a *= 0.5;
    }
    return s / 0.9375;
}
// a coal's jitter (x, y) and tint
vec3 cellRand(ivec2 c) {
    return vec3(hashCell(c + ivec2(0, 8192)), hashCell(c + ivec2(8192, 0)), hashCell(c + ivec2(8192, 8192)));
}
vec2 vpt(ivec2 c) {
    vec3  r = cellRand(c);
    float a = mod((128.0 + floor(r.z * 256.0)) * theta, 6.2831853) + 6.2831853 * r.z;
    return 0.35 + 0.3 * r.xy + 0.35 * vec2(sin(a), cos(a));
}
vec3 vor(vec2 g) {
    vec2  gfl = floor(g);
    ivec2 gi  = ivec2(gfl);
    vec2  gf  = g - gfl;
    ivec2 mg  = ivec2(0);
    vec2  mr  = vec2(0.0);
    float f1  = 99.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            ivec2 o2 = ivec2(i, j);
            vec2  r  = vec2(o2) + vpt(gi + o2) - gf;
            float d  = dot(r, r);
            if (d < f1) {
                f1 = d;
                mr = r;
                mg = o2;
            }
        }
    float md = 99.0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            ivec2 o2 = mg + ivec2(i, j);
            vec2  r  = vec2(o2) + vpt(gi + o2) - gf;
            if (dot(mr - r, mr - r) > 1e-5)
                md = min(md, dot(0.5 * (mr + r), normalize(r - mr)));
        }
    return vec3(sqrt(f1), md, cellRand(gi + mg).z);
}
vec4 coal(vec2 p) {
    vec2  w    = vec2(fnI(p * 3.0 + 1.7, 0), fnI(p * 3.0 + 8.2, 0)) - 0.5;
    vec2  w2   = vec2(fbmT(p * 40.0 + 3.1, creepI[0], creepF[0]), fbmT(p * 40.0 + 11.7, creepI[1], creepF[1])) - 0.5;
    vec3  v    = vor(p * 24.0 + w * 5.0 + w2 * 0.9);
    float edge = v.y;
    float wd   = max(0.045, 1.5 * fwidth(edge));
    float brk  = smoothstep(0.25, 0.7, fbmT(p * 14.0 + 5.0, creepI[2], creepF[2]));
    return vec4(edge, v.z, brk, wd);
}
)GLSL";

// One strip of a key (COAL_BAKE): the coal layer at the picture's size, no margin (the
// picture samples it at its own uv): r = edge / 0.5, g = tint, b = break-up, a = crack
// width / 0.5. gl_FragCoord is in window coordinates, so the strip's offset is in it.
inline constexpr const char* BAKE = R"GLSL(
uniform float invShort;
layout(location = 0) out vec4 o;
void main() {
    vec4 k = coal(gl_FragCoord.xy * invShort);
    o      = vec4(clamp(k.x / 0.5, 0.0, 1.0), k.y, k.z, clamp(k.w / 0.5, 0.0, 1.0));
}
)GLSL";

// The picture (firePicSrc, TEX 3, from above). The heat by the B-spline (16 taps, the
// page's tcub: the same cubic B-spline from four bilinear taps), the bed's glow, the coal
// layer (BAKED: one tap of each shown key, blended; else per pixel, the sketch's path),
// the coals' pulse, the cracks lit by the heat and by the proximity (BAKED: the blurred
// heat, bilinear as the page; else the sketch's 32-tap ring, bilinear by hand), the
// coral ramp, the dither after the 8-bit bake. The pulse sin(time 2e-3 (0.5 + id) + 40 id)
// with id at the key's 8 bits, k / 255: (255 + 2k) pulse, pulse = time 2e-3 / 510 mod 2 pi
// from C++. fade scales the picture to black.
inline constexpr const char* PICTURE = R"GLSL(
uniform sampler2D heat;
#if BAKED
uniform sampler2D prox;
uniform sampler2D keyA;
uniform sampler2D keyB;
uniform float blend;
#endif
uniform vec2 view;
uniform vec4 picBed;
uniform float pulseTheta;
uniform float fade;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

const vec3 BG = vec3(0.07, 0.065, 0.08);
const vec3 K1 = vec3(0.584, 0.322, 0.318), K2 = vec3(1.0, 0.435, 0.380), K3 = vec3(0.980, 0.604, 0.522), K4 = vec3(1.0, 0.745, 0.596);
const float STR = 0.35, REACH = 14.0;
vec3 ramp(float t) {
    t = clamp(t, 0.0, 1.0);
    return t < 0.25 ? mix(K1, K2, t / 0.25) : t < 0.6 ? mix(K2, K3, (t - 0.25) / 0.35) : mix(K3, K4, (t - 0.6) / 0.4);
}
void main() {
    vec2  t    = vec2(v_texcoord.x, 1.0 - v_texcoord.y); // the one flip: y up
    vec2  pix  = t * view;
    vec2  hs   = vec2(textureSize(heat, 0));
    float T    = bicubic(heat, t * hs, true).r;
    float bv   = bedv(t, picBed);
    float glow = smoothstep(LO - 0.25, LO, bv);
#if BAKED
    vec4  kc   = mix(texture(keyA, t), texture(keyB, t), blend);
    float edge = kc.r * 0.5, id = kc.g, brk = kc.b, wd = kc.a * 0.5;
#else
    vec4  kc   = coal(pix / min(view.x, view.y));
    float edge = kc.x, id = kc.y, brk = kc.z, wd = kc.w;
#endif
    float idq   = floor(id * 255.0 + 0.5);
    float pulse = 0.55 + 0.45 * sin(mod((255.0 + 2.0 * idq) * pulseTheta, 6.2831853) + id * 40.0);
    float soft  = smoothstep(0.0, 0.12, edge), idm = mix(0.5, id, soft);
    float Td    = T * (0.55 + 0.9 * mix(0.4, id * pulse, soft));
    float crack = STR * min(1.0, 0.07 / wd) * (1.0 - smoothstep(0.0, wd, edge)) * mix(0.25, 1.0, brk);
    vec3  cB    = BG + K1 * 0.25 * glow * (0.4 + 0.6 * idm);
    cB          = mix(cB, ramp(Td * 0.35), smoothstep(0.05, 0.4, Td) * 0.8);
#if BAKED
    float Pn = smoothstep(0.0, 0.35, bilinear(prox, t * hs, true).r);
#else
    float P = 0.0, Wt = 0.0;
    for (int k = 0; k < 32; k++) {
        float a  = float(k) * 0.7853982 + float(k / 8) * 0.39;
        float r  = REACH * (float(k / 8) + 1.0) / 4.0;
        float wk = exp(-2.0 * r * r / (REACH * REACH));
        P += wk * bilinear(heat, t * hs + r * vec2(cos(a), sin(a)), true).r;
        Wt += wk;
    }
    float Pn = smoothstep(0.0, 0.35, P / Wt);
#endif
    cB = mix(cB, ramp(max(T, 0.12 + 0.5 * Pn)), crack * max(smoothstep(0.02, 0.3, T), Pn));
    cB += K2 * 0.25 * STR * (1.0 - smoothstep(0.0, 0.2, edge)) * smoothstep(0.1, 0.6, T);
    vec3 col = cB + (fh(gl_FragCoord.xy * 0.731) - 0.5) / 255.0;
    fragColor = vec4(col * fade, 1.0);
}
)GLSL";

} // namespace EmbersShaders
