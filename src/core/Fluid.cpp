#include "core/Fluid.hpp"
#include "core/Fields.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace {

constexpr const char* LATTICE = R"GLSL(
const int CX[9] = int[9](0, 1, 0, -1, 0, 1, -1, -1, 1);
const int CY[9] = int[9](0, 0, 1, 0, -1, 1, 1, -1, -1);
const int OPP[9] = int[9](0, 3, 4, 1, 2, 7, 8, 5, 6);
const float W[9] = float[9](4.0 / 9.0, 1.0 / 9.0, 1.0 / 9.0, 1.0 / 9.0, 1.0 / 9.0, 1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0, 1.0 / 36.0);
float feq(int q, float rho, vec2 u) {
    float cu = float(CX[q]) * u.x + float(CY[q]) * u.y;
    return W[q] * rho * (1.0 + 3.0 * cu + 4.5 * cu * cu - 1.5 * dot(u, u));
}
uniform sampler2D F0;
uniform sampler2D F1;
uniform sampler2D F2;
uniform ivec2 size;
uniform float omega;
const float RHO_FLOOR = 1.0e-3;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec2 o3;
float fetchQ(int q, ivec2 c) {
    if (q < 4)
        return texelFetch(F0, c, 0)[q];
    if (q < 8)
        return texelFetch(F1, c, 0)[q - 4];
    return texelFetch(F2, c, 0).x;
}
)GLSL";

// lbmStepSrc of the sketch: pull streaming (half-way bounce-back off the walls),
// moments, force, collision towards the equilibrium at the shifted velocity
// u + F / (omega rho) (capped at 0.2), out the half-force-corrected velocity.
// Mass is conserved to rounding: streaming and bounce-back move populations, the
// force and the cap change only the equilibrium's velocity, and the collision
// gives the rest population the remainder of rho.
constexpr const char* STEP_MAIN = R"GLSL(
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    float f[9];
    for (int q = 0; q < 9; q++) {
        int sx = c.x - CX[q];
        if (sx < 0)
            sx += size.x;
        else if (sx >= size.x)
            sx -= size.x;
        int sy = c.y - CY[q];
#if WALLS
        if (sy < 0 || sy >= size.y) {
            f[q] = fetchQ(OPP[q], c);
            continue;
        }
#else
        if (sy < 0)
            sy += size.y;
        else if (sy >= size.y)
            sy -= size.y;
#endif
        f[q] = fetchQ(q, ivec2(sx, sy));
    }
    float rho = 0.0;
    vec2  m   = vec2(0.0);
    for (int q = 0; q < 9; q++) {
        rho += f[q];
        m += f[q] * vec2(float(CX[q]), float(CY[q]));
    }
    // divisions by the density at least RHO_FLOOR: a near-empty cell gives a large but
    // finite velocity (the health check sees the density) instead of an infinity that
    // spreads; at any density a run reaches this is rho itself, bit for bit
    float rd  = max(rho, RHO_FLOOR);
    vec2  u   = m / rd;
    vec2  F   = force(c, u, rho);
    vec2  ueq = u + F / (omega * rd);
    float s   = length(ueq);
    if (s > 0.2)
        ueq *= 0.2 / s;
    // BGK on the eight moving populations; the rest population takes what is left
    // of rho. In exact arithmetic this is the same collision (feq0 = rho - the other
    // eight equilibria), but in fp32 the nine rounded equilibria do not sum to rho
    // (the weights sum to 1 + 7.45e-9, and W*rho rounds with a bias that depends on
    // rho's binade), which added about 1.4e-8 to 2.8e-8 of mass per cell and step.
    float moving = 0.0;
    for (int q = 1; q < 9; q++) {
        f[q] += omega * (feq(q, rho, ueq) - f[q]);
        moving += f[q];
    }
    f[0] = rho - moving;
    vec2 uo = u + 0.5 * F / rd;
    o0 = vec4(f[0], f[1], f[2], f[3]);
    o1 = vec4(f[4], f[5], f[6], f[7]);
    o2 = vec4(f[8], uo, rho);
    o3 = uo;
}
)GLSL";

constexpr const char* NO_FORCE = "vec2 force(ivec2 c, vec2 u, float rho) { return vec2(0.0); }\n";

} // namespace

CFluid::~CFluid() {
    release();
}

void CFluid::release() {
    for (auto& fb : m_fb)
        Gl::destroyFramebuffer(fb);
    for (auto& set : m_tex)
        for (auto& tex : set)
            Gl::destroyTexture(tex);
    Gl::destroy(m_step);
    m_cur = 0;
}

bool CFluid::build(const SFluidSpec& spec, std::string* error) {
    release();
    m_spec = spec;
    if (spec.w < 3 || spec.h < 3) {
        if (error)
            *error = std::format("a fluid lattice of {}x{} cells", spec.w, spec.h);
        return false;
    }
    if (!floatTargetsAvailable(error))
        return false;

    const std::string src = std::format("#define WALLS {}\n", spec.wallsY ? 1 : 0) + LATTICE + GLSL_PDELTA + GLSL_BILINEAR + GLSL_BICUBIC +
        (spec.forceGlsl.empty() ? std::string(NO_FORCE) : spec.forceGlsl) + STEP_MAIN;
    std::string log;
    auto        program = Gl::compile(src, &log);
    if (!program) {
        if (error)
            *error = std::format("the fluid's step shader (with its force) failed to compile: {}", log);
        return false;
    }
    m_step   = *program;
    m_uF[0]  = m_step.uniform("F0");
    m_uF[1]  = m_step.uniform("F1");
    m_uF[2]  = m_step.uniform("F2");
    m_uSize  = m_step.uniform("size");
    m_uOmega = m_step.uniform("omega");

    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < 3; k++)
            m_tex[i][k] = Gl::texture(GL_RGBA32F, spec.w, spec.h, GL_NEAREST);
        m_tex[i][3] = Gl::texture(GL_RG32F, spec.w, spec.h, GL_NEAREST);
        m_fb[i]     = Gl::framebuffer({m_tex[i][0], m_tex[i][1], m_tex[i][2], m_tex[i][3]});
    }
    if (!m_fb[0] || !m_fb[1]) {
        if (error)
            *error = std::format("the fluid's {}x{} float framebuffers (3 x RGBA32F + RG32F) are incomplete", spec.w, spec.h);
        release();
        return false;
    }
    init();
    return true;
}

void CFluid::init() {
    if (!m_fb[0])
        return;
    // the equilibrium at rest, rho 1: the weights themselves
    const GLfloat f03[4] = {4.0f / 9, 1.0f / 9, 1.0f / 9, 1.0f / 9};
    const GLfloat f47[4] = {1.0f / 9, 1.0f / 36, 1.0f / 36, 1.0f / 36};
    const GLfloat f8u[4] = {1.0f / 36, 0, 0, 1};
    const GLfloat zero[4] = {0, 0, 0, 0};
    for (GLuint fb : m_fb) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glClearBufferfv(GL_COLOR, 0, f03);
        glClearBufferfv(GL_COLOR, 1, f47);
        glClearBufferfv(GL_COLOR, 2, f8u);
        glClearBufferfv(GL_COLOR, 3, zero);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_cur = 0;
}

const Gl::SProgram& CFluid::program() const {
    return m_step;
}

void CFluid::step() {
    if (!m_step.id || !m_fb[0])
        return;
    const int nxt = 1 - m_cur;
    Gl::use(m_step);
    for (int k = 0; k < 3; k++) {
        Gl::bind(k, m_tex[m_cur][k]);
        glUniform1i(m_uF[k], k);
    }
    glUniform2i(m_uSize, m_spec.w, m_spec.h);
    glUniform1f(m_uOmega, m_spec.omega);
    Gl::pass(m_step, m_fb[nxt], 0, 0, m_spec.w, m_spec.h);
    m_cur = nxt;
}

GLuint CFluid::velocity() const {
    return m_tex[m_cur][3];
}

CFluid::SStats CFluid::stats() {
    SStats s{0, 0, 0, 0, true};
    if (!m_fb[0])
        return s;
    const size_t       n = static_cast<size_t>(m_spec.w) * m_spec.h;
    std::vector<float> px(n * 4);
    {
        Gl::CPackState pack;
        GLint          readFb = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fb[m_cur]);
        glReadBuffer(GL_COLOR_ATTACHMENT2);
        glReadPixels(0, 0, m_spec.w, m_spec.h, GL_RGBA, GL_FLOAT, px.data());
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFb));
    }

    bool   finite = true;
    double sum = 0, rsum = 0;
    float  umax = 0, rmin = INFINITY, rmax = -INFINITY;
    for (size_t i = 0; i < n; i++) {
        const float ux = px[i * 4 + 1], uy = px[i * 4 + 2], rho = px[i * 4 + 3];
        if (!std::isfinite(ux) || !std::isfinite(uy) || !std::isfinite(rho)) {
            finite = false;
            continue;
        }
        const float sp = std::hypot(ux, uy);
        umax           = std::max(umax, sp);
        sum += sp;
        rmin = std::min(rmin, rho);
        rmax = std::max(rmax, rho);
        rsum += rho;
    }
    s.umax    = umax;
    s.umean   = static_cast<float>(sum / static_cast<double>(n));
    s.rhoMin  = rmin;
    s.rhoMax  = rmax;
    s.rhoMean = static_cast<float>(rsum / static_cast<double>(n));
    s.bad     = !finite || umax > 0.4f || !(rmin > 0.0f);
    return s;
}
