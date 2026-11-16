#pragma once

// Fields carried by a fluid, and the GLSL helpers every look's passes share.
//
// Conventions (the same in CFluid):
// - A texture's row 0 is v_texcoord.y 0 and gl_FragCoord.y 0.5: the top of the
//   picture (Gl::FULL keeps it there). load() takes row 0 first.
// - Positions are in cell-centre coordinates: texel (i, j) covers [i, i+1) x [j, j+1)
//   and its centre is (i + 0.5, j + 0.5), so gl_FragCoord.xy of a full-target pass is
//   the centre of the cell it writes.
// - Float textures are never read with texture() and hardware filtering (float32
//   linear filtering is an extension): every read is texelFetch, and filtering is
//   written by hand (bilinear, bicubic). Every texture made here is GL_NEAREST.
// - Periodic in x always; in y periodic or clamped to the edge rows.

#include "core/Compound.hpp"
#include "gl/Gl.hpp"

#include <string>
#include <vector>

// vec2 pdelta(vec2 a, vec2 b): a - b, the shortest way round on both axes of a periodic
// domain of `size` cells. Uses `uniform ivec2 size`, which the including shader declares
// (CFluid's step and CField's passes do: the lattice and the field size).
inline constexpr const char* GLSL_PDELTA = R"GLSL(
vec2 pdelta(vec2 a, vec2 b) { vec2 s = vec2(size); vec2 d = a - b; return d - s * floor(d / s + 0.5); }
)GLSL";

// vec4 bilinear(sampler2D t, vec2 cell, bool wrapY): t at a cell-centre position, four
// texelFetch taps. One function returning vec4 (GLSL cannot overload on the return
// type): take .r / .rg of it for a one- or two-channel texture. x wraps; y wraps with
// wrapY, else the edge rows repeat (as CLAMP_TO_EDGE). Also defines the index helpers
// wrapIndex(i, n) (i mod n for any sign, integer arithmetic only, no % of a negative, no
// division within a period of [0, n)),
// cellIndex(ivec2 i, ivec2 size, bool wrapY) (one cell) and cellPair(ivec2 i, ivec2
// size, bool wrapY) = (x0, x1, y0, y1), the columns i.x, i.x + 1 and rows i.y, i.y + 1
// of a 2x2 block: one wrapIndex per axis, the +1 by a conditional -n (no division).
inline constexpr const char* GLSL_BILINEAR = R"GLSL(
int wrapIndex(int i, int n) {
    // the indices met in practice are at most one period outside: no division for them
    // (an integer division is the dearest thing in a tap on some GPUs); the same value
    if (i >= -n && i < 2 * n)
        return i < 0 ? i + n : i >= n ? i - n : i;
    if (i < 0)
        i += n * ((n - 1 - i) / n);
    return i - n * (i / n);
}
ivec2 cellIndex(ivec2 i, ivec2 sz, bool wrapY) {
    return ivec2(wrapIndex(i.x, sz.x), wrapY ? wrapIndex(i.y, sz.y) : clamp(i.y, 0, sz.y - 1));
}
int nextIndex(int i, int n) {
    return i + 1 == n ? 0 : i + 1;
}
ivec4 cellPair(ivec2 i, ivec2 sz, bool wrapY) {
    int x0 = wrapIndex(i.x, sz.x);
    if (wrapY) {
        int y0 = wrapIndex(i.y, sz.y);
        return ivec4(x0, nextIndex(x0, sz.x), y0, nextIndex(y0, sz.y));
    }
    return ivec4(x0, nextIndex(x0, sz.x), clamp(i.y, 0, sz.y - 1), clamp(i.y + 1, 0, sz.y - 1));
}
vec4 bilinear(sampler2D t, vec2 cell, bool wrapY) {
    ivec2 sz = textureSize(t, 0);
    vec2  p  = cell - 0.5;
    vec2  b  = floor(p);
    vec2  f  = p - b;
    ivec4 k  = cellPair(ivec2(b), sz, wrapY);
    vec4 a00 = texelFetch(t, k.xz, 0);
    vec4 a10 = texelFetch(t, k.yz, 0);
    vec4 a01 = texelFetch(t, k.xw, 0);
    vec4 a11 = texelFetch(t, k.yw, 0);
    return mix(mix(a00, a10, f.x), mix(a01, a11, f.x), f.y);
}
)GLSL";

// vec4 bicubic(sampler2D t, vec2 cell, bool wrapY): the uniform cubic B-spline over the
// 4x4 cells around the position, 16 texelFetch taps; smooth (C2) but not interpolating:
// it softens as a B-spline does. Same conventions as bilinear; GLSL_BILINEAR goes first.
// The four columns and four rows are wrapped once each (one wrapIndex per axis, then
// conditional steps), not per tap. Also defines bicubicGrad(t, cell, wrapY, out dx,
// out dy): the same value, and in dx, dy the B-spline's exact derivatives along x and y
// (per cell, every channel) from the same 16 taps.
inline constexpr const char* GLSL_BICUBIC = R"GLSL(
void bsplineWeights(vec2 f, out vec2 w[4]) {
    vec2 f2 = f * f, f3 = f2 * f, g = 1.0 - f;
    w[0] = g * g * g / 6.0;
    w[1] = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
    w[2] = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
    w[3] = f3 / 6.0;
}
// the columns i.x - 1 .. i.x + 2 and rows i.y - 1 .. i.y + 2 of a 4x4 block
void cellQuad(ivec2 i, ivec2 sz, bool wrapY, out ivec4 xs, out ivec4 ys) {
    xs.x = wrapIndex(i.x - 1, sz.x);
    xs.y = nextIndex(xs.x, sz.x);
    xs.z = nextIndex(xs.y, sz.x);
    xs.w = nextIndex(xs.z, sz.x);
    if (wrapY) {
        ys.x = wrapIndex(i.y - 1, sz.y);
        ys.y = nextIndex(ys.x, sz.y);
        ys.z = nextIndex(ys.y, sz.y);
        ys.w = nextIndex(ys.z, sz.y);
    } else
        ys = clamp(ivec4(i.y - 1, i.y, i.y + 1, i.y + 2), 0, sz.y - 1);
}
vec4 bicubic(sampler2D t, vec2 cell, bool wrapY) {
    ivec2 sz = textureSize(t, 0);
    vec2  p  = cell - 0.5;
    vec2  b  = floor(p);
    vec2  w[4];
    bsplineWeights(p - b, w);
    ivec4 xs, ys;
    cellQuad(ivec2(b), sz, wrapY, xs, ys);
    vec4 s = vec4(0.0);
    for (int y = 0; y < 4; y++) {
        vec4 row = vec4(0.0);
        for (int x = 0; x < 4; x++)
            row += w[x].x * texelFetch(t, ivec2(xs[x], ys[y]), 0);
        s += w[y].y * row;
    }
    return s;
}
vec4 bicubicGrad(sampler2D t, vec2 cell, bool wrapY, out vec4 dx, out vec4 dy) {
    ivec2 sz = textureSize(t, 0);
    vec2  p  = cell - 0.5;
    vec2  b  = floor(p);
    vec2  f  = p - b;
    vec2  w[4];
    bsplineWeights(f, w);
    // d/df of the weights
    vec2 g = 1.0 - f;
    vec2 d[4];
    d[0] = -0.5 * g * g;
    d[1] = 1.5 * f * f - 2.0 * f;
    d[2] = -1.5 * f * f + f + 0.5;
    d[3] = 0.5 * f * f;
    ivec4 xs, ys;
    cellQuad(ivec2(b), sz, wrapY, xs, ys);
    vec4 s = vec4(0.0);
    dx = vec4(0.0);
    dy = vec4(0.0);
    for (int y = 0; y < 4; y++) {
        vec4 row = vec4(0.0), rowDx = vec4(0.0);
        for (int x = 0; x < 4; x++) {
            vec4 v = texelFetch(t, ivec2(xs[x], ys[y]), 0);
            row += w[x].x * v;
            rowDx += d[x].x * v;
        }
        s += w[y].y * row;
        dx += w[y].y * rowDx;
        dy += d[y].y * row;
    }
    return s;
}
)GLSL";

// Float colour buffers (R32F, RG32F, RGBA32F and the 16F ones) need
// GL_EXT_color_buffer_float. false and *error when the context does not offer it.
bool floatTargetsAvailable(std::string* error);

// postGlsl: `vec4 post(ivec2 c, vec4 v)` + its uniforms, run on every cell after the
// transport (v the transported, clamped value; the result is stored). Empty = identity.
// internal: GL_R32F, GL_RG32F, GL_RGBA32F, GL_R16F, GL_RG16F or GL_RGBA16F.
struct SFieldSpec {
    int         w, h;
    GLenum      internal;
    bool        wrapY;
    std::string postGlsl;
};

// One transported field: semi-Lagrangian MacCormack (forward trace, backward trace,
// correction clamped to the four cells around the departure point), then post().
//
// Texture units: step() binds units 0..3 (field, forward, backward, velocity); a
// post() that samples its own textures takes them on unit 4 and up, bound by the
// caller before step(). Uniform names the passes own (a post must not declare them):
// size, fieldSrc, fieldHat, fieldBar, fieldVel, velSize, fieldDt, fieldDir. A post can
// read size, fieldSrc (the field before this step, unit 0), fieldVel (unit 3), fieldDt
// and the prelude's helpers (displacement(c), pdelta, bilinear, bicubic, WRAPY).
class CField {
  public:
    CField() = default;
    ~CField();
    CField(const CField&)            = delete;
    CField& operator=(const CField&) = delete;

    bool build(const SFieldSpec&, std::string* error); // (re)makes everything; contents 0
    void load(const std::vector<float>& rgba);          // w*h*4 floats, row 0 (the top) first; other sizes are ignored
    const Gl::SProgram& program() const;                // the final (post) pass: set post uniforms on it
    // velocity: RG, lattice cells per fluid step, velW x velH (CFluid::velocity()).
    // Sampled bilinearly by cell centres when its size differs from the field's, and
    // scaled by the size ratio; dt multiplies the displacement (in fluid steps).
    void   step(GLuint velocity, int velW, int velH, float dt);
    GLuint texture() const;
    GLuint framebuffer() const { return m_fb[m_cur]; } // texture()'s, for a read-back
    int    width() const { return m_spec.w; }
    int    height() const { return m_spec.h; }

  private:
    void release();

    SFieldSpec   m_spec{};
    GLuint       m_tex[2] = {}, m_hat = 0, m_bar = 0;
    GLuint       m_fb[2] = {}, m_fbHat = 0, m_fbBar = 0;
    int          m_cur = 0;
    Gl::SProgram m_advect, m_mac;
    struct SUniforms {
        GLint src = -1, hat = -1, bar = -1, vel = -1, size = -1, velSize = -1, dt = -1, dir = -1;
    } m_advU, m_macU;
};
