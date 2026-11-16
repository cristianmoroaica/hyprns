#pragma once

// GLSL of the look `pattern` (Pattern.hpp): the stirrers' force on the fluid, the
// Gray-Scott step with its carry, and the picture. Ported from sketchRD in
// reference/d2e-sketch.html (left card: RD_LOOSE1, sharp, uneven, events, vel 1).
//
// Orientation (the convention of every look ported from a sketch): the lattice and the
// chemistry are in the sketch's own coordinates, row 0 at the BOTTOM of the picture (the
// sketch draws its lattice straight to a WebGL canvas, whose row 0 is at the bottom), so
// every position, velocity and light direction of the sketch is used as it is. The
// picture pass is the only place that flips: cell y = (1 - v_texcoord.y) * height.

namespace PatternShaders {

// The three stirrers and the gust (CFluid force snippet; pdelta from Fields.hpp).
// stir[i] = (x, y, vx, vy) in cells and cells per fluid step, env[i] its strength (0 =
// off: skipped, which adds exactly nothing). The sketch's force, term by term.
inline constexpr const char* FORCE = R"GLSL(
uniform vec4 stir[4];
uniform vec4 env;
uniform float drag;
vec2 force(ivec2 c, vec2 u, float rho) {
    vec2 p = vec2(c) + 0.5;
    vec2 F = -drag * u;
    for (int i = 0; i < 4; i++) {
        if (env[i] > 0.0) {
            vec2  d = pdelta(p, stir[i].xy);
            float g = exp(-dot(d, d) / 256.0);
            F += 0.05 * env[i] * g * (stir[i].zw - u);
        }
    }
    return F;
}
)GLSL";

// One Gray-Scott step on the RGBA32F state (a, b, act, speed), one pass:
// - the 3x3 block around the cell is fetched once (9 taps) and gives both the sketch's
//   9-point Laplacian and its semi-Lagrangian carry: the carry samples at c - 0.5 u,
//   and with |0.5 u| <= 1 cell its bilinear taps are four of those nine, interpolated
//   here from registers (the displacement is clamped to one cell: a fluid that fast has
//   blown up and is reset anyway);
// - plus the velocity at the cell (1 tap): 10 fetches per cell and step.
// The sketch's order: Laplacian of the unmoved neighbours, reaction on the carried value,
// sources, act from the unmoved cell. `speed` = |u| of this fluid step, for the picture.
// Feed/kill: the sketch's two octaves of drifting value noise blended between three
// points; the drift offsets come from C++ split into integer cells (n1i, n2i) and a
// fraction (n1f, n2f), and the lattice corners are hashed as integers, so the noise
// stays exact however long it runs (the sketch's float hash of a growing position does
// not).
inline constexpr const char* STEP = R"GLSL(
uniform sampler2D chem;
uniform sampler2D vel;
uniform ivec2 size;
uniform float invShort;
uniform ivec2 n1i;
uniform vec2 n1f;
uniform ivec2 n2i;
uniform vec2 n2f;
uniform vec4 seed;
layout(location = 0) out vec4 o;

float hashCell(ivec2 i) {
    uint h = uint(i.x) * 0x8da6b343u ^ uint(i.y) * 0xd8163841u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h >> 8) * (1.0 / 16777216.0);
}
// value noise at base + q (base: whole cells, q: the rest)
float vnoise(ivec2 base, vec2 q) {
    vec2  fl = floor(q);
    vec2  f  = q - fl;
    ivec2 i  = base + ivec2(fl);
    f        = f * f * (3.0 - 2.0 * f);
    return mix(mix(hashCell(i), hashCell(i + ivec2(1, 0)), f.x), mix(hashCell(i + ivec2(0, 1)), hashCell(i + ivec2(1, 1)), f.x), f.y);
}
vec2 params(vec2 p) {
    float n = vnoise(n1i, p * 1.6 + n1f) * 0.65 + vnoise(n2i, p * 3.3 + n2f) * 0.35;
    n       = smoothstep(0.2, 0.8, n);
    const vec2 P0 = vec2(0.0367, 0.0649), P1 = vec2(0.050, 0.0632), P2 = vec2(0.031, 0.0595);
    return n < 0.5 ? mix(P0, P1, n * 2.0) : mix(P1, P2, n * 2.0 - 1.0);
}

void main() {
    ivec2 c  = ivec2(gl_FragCoord.xy);
    int   xm = c.x == 0 ? size.x - 1 : c.x - 1, xp = c.x + 1 == size.x ? 0 : c.x + 1;
    int   ym = c.y == 0 ? size.y - 1 : c.y - 1, yp = c.y + 1 == size.y ? 0 : c.y + 1;
    vec4 vMM = texelFetch(chem, ivec2(xm, ym), 0), v0M = texelFetch(chem, ivec2(c.x, ym), 0), vPM = texelFetch(chem, ivec2(xp, ym), 0);
    vec4 vM0 = texelFetch(chem, ivec2(xm, c.y), 0), v00 = texelFetch(chem, c, 0), vP0 = texelFetch(chem, ivec2(xp, c.y), 0);
    vec4 vMP = texelFetch(chem, ivec2(xm, yp), 0), v0P = texelFetch(chem, ivec2(c.x, yp), 0), vPP = texelFetch(chem, ivec2(xp, yp), 0);

    vec2 lap = 0.2 * (vP0.xy + vM0.xy + v0P.xy + v0M.xy) + 0.05 * (vPP.xy + vMM.xy + vPM.xy + vMP.xy) - v00.xy;

    // the carry: the state at c - 0.5 u, bilinear (the sketch's texture() at (c + 0.5 - u) / size)
    vec2  u0 = texelFetch(vel, c, 0).xy;
    vec2  s  = clamp(-0.5 * u0, -1.0, 1.0);
    vec2  w  = abs(s);
    bool  lx = s.x < 0.0, ly = s.y < 0.0;
    vec2  hM = mix(v0M.xy, lx ? vMM.xy : vPM.xy, w.x);
    vec2  h0 = mix(v00.xy, lx ? vM0.xy : vP0.xy, w.x);
    vec2  hP = mix(v0P.xy, lx ? vMP.xy : vPP.xy, w.x);
    vec2  v  = mix(h0, ly ? hM : hP, w.y);

    vec2  fk = params(vec2(c) * invShort);
    float a = v.x, b = v.y, r = a * b * b;
    a += 1.0 * lap.x - r + fk.x * (1.0 - a);
    b += 0.5 * lap.y + r - (fk.x + fk.y) * b;
    if (seed.z > 0.0) {
        float m = 1.0 - smoothstep(seed.z - 3.0, seed.z, distance(vec2(c), seed.xy));
        if (seed.w > 0.5)
            b = mix(b, 0.5, m);
        else {
            b = mix(b, 0.0, m);
            a = mix(a, 1.0, m);
        }
    }
    float act = mix(v00.z, abs(b - v00.y) * 300.0, 0.04);
    o = vec4(clamp(a, 0.0, 1.0), clamp(b, 0.0, 1.0), act, length(u0));
}
)GLSL";

// The picture at monitor resolution: the state by the B-spline (16 taps, which also give
// the exact gradient), then the sketch's pShow (SHARP 1, VEL 1, LO 0.22, ESP espresso):
// threshold 0.19 antialiased by fwidth, relief, speed-driven colour and opacity, the
// glow, the background. Everything is in lattice cells or per screen pixel (fwidth), so
// the web keeps its size relative to the short side at any resolution or quality.
// fade scales the picture to black, so a switch has no step.
inline constexpr const char* PICTURE = R"GLSL(
uniform sampler2D chem;
uniform float fade;
in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 sz   = vec2(textureSize(chem, 0));
    vec2 cell = vec2(v_texcoord.x, 1.0 - v_texcoord.y) * sz; // the one flip: lattice row 0 is the bottom
    vec4 dx, dy;
    vec4 s = bicubicGrad(chem, cell, true, dx, dy);
    float b = s.y;
    // the sketch's central differences over +-1 cell: twice the derivative per cell
    float hx = 2.0 * dx.y, hy = 2.0 * dy.y;
    vec3  n = normalize(vec3(-hx * 4.0, -hy * 4.0, 1.0));
    vec3  L = normalize(vec3(-0.5, 0.6, 0.65));
    float w = max(fwidth(b) * 1.2, 0.004);
    float k = smoothstep(0.19 - w, 0.19 + w, b);
    float v = smoothstep(0.002, 0.04, s.w);
    vec3 body = mix(vec3(0.33, 0.21, 0.14), vec3(0.80, 0.64, 0.36), smoothstep(0.1, 0.9, v));
    k *= mix(0.22, 1.0, smoothstep(0.0, 0.6, v));
    vec3 col = mix(vec3(0.07, 0.065, 0.08), body, k);
    col      = mix(col, min(body * 1.3, vec3(1.0)), smoothstep(0.3, 0.45, b) * k);
    float sh = 0.7 + 0.45 * dot(n, L);
    float sp = pow(max(dot(reflect(-L, n), vec3(0.0, 0.0, 1.0)), 0.0), 18.0);
    col      = mix(col, col * sh + 0.1 * sp, k);
    col += vec3(0.85, 0.66, 0.36) * clamp(s.z, 0.0, 1.0) * (0.25 + 0.3 * k);
    fragColor = vec4(col * fade, 1.0);
}
)GLSL";

} // namespace PatternShaders
