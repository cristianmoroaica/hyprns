#include "looks/Pattern.hpp"
#include "core/Fields.hpp"
#include "core/Sequence.hpp"
#include "looks/PatternShaders.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace {

// The sketch's layout is for a 400x225 lattice.
constexpr double SKETCH_W = 400.0, SKETCH_H = 225.0;
constexpr double TAU      = 6.283185307179586;

// The sketch's ST: centre, amplitude, angular speeds (irrational ratios), phases, and the
// unevenness' speeds (env), per fluid step.
constexpr std::array<std::array<double, 10>, 3> STIRRERS = {{
    {100, 112, 120, 70, 6.2e-4, 6.2e-4 * 1.6180339887, 0.0, 1.3, 1.8e-3, 1.8e-3 * 0.6180339887},
    {300, 112, 110, 80, 5.2e-4 * 1.4142135623, 5.2e-4, 2.1, 0.4, 1.5e-3, 1.5e-3 * 1.4142135623},
    {200, 60, 160, 50, 4.4e-4, 4.4e-4 * 2.2360679775, 4.0, 3.3, 2.1e-3, 2.1e-3 * 0.7548776662},
}};

constexpr float  OMEGA = 1.85f, DRAG = 6e-4f; // the fluid (uneven: drag 6e-4)
constexpr int    SEED_EVERY = 400;            // chemistry steps between the small periodic seeds
constexpr double SEED_R = 3, BURST_R = 16, WIPE_R = 48, GUST_SPEED = 0.13, GUST_STEPS = 1800;
constexpr int    BURST_N = 3, WIPE_N = 40;   // chemistry steps a burst / wipe is applied
constexpr double FIRST_EVENT = 9000, EVENT_MIN = 15000, EVENT_SPREAD = 15000; // chemistry steps
constexpr double GOLDEN_ANGLE = 2.39996, THIRD = 0.5698402910;

// Health: the fraction of cells over the threshold that counts as a vanished or a
// saturated web (the sketch runs at 0.2 to 0.4).
constexpr float COVER_DEAD = 0.002f, COVER_FULL = 0.85f;
// a fluid speed no stable run reaches (the limit CFluid::stats() uses)
constexpr float UMAX_BLOWN = 0.4f;

// The health check's reduction, per cell: |u| (the chemistry's .w, the last fluid step's
// speed), the density's min and max; the cells over the web threshold, the glow and
// the density summed; a, b, glow and density must be finite.
constexpr const char* HEALTH_CELL = R"GLSL(
uniform sampler2D chemTex;    // a, b, glow, |u|
uniform sampler2D fluidState; // f8, ux, uy, rho
void cell(ivec2 c, out vec4 mx, out vec3 sm, out vec4 chk) {
    vec4  k   = texelFetch(chemTex, c, 0);
    float rho = texelFetch(fluidState, c, 0).w;
    mx  = vec4(k.w, -rho, rho, 0.0);
    sm  = vec3(k.y > 0.19 ? 1.0 : 0.0, k.z, rho);
    chk = vec4(k.xyz, rho);
}
)GLSL";

const char* kindName(int kind) {
    switch (kind) {
        case 0: return "burst";
        case 1: return "wipe";
        case 2: return "gust";
        default: return "none";
    }
}

} // namespace

CPatternLook::CPatternLook(uint64_t seed) : m_seed(seed) {
    uint64_t s = seed;
    for (size_t i = 0; i < m_stirrers.size(); i++) {
        const auto& q = STIRRERS[i];
        m_stirrers[i] = {q[0], q[1], q[2], q[3], q[4], q[5], q[6] + TAU * uniform01(s), q[7] + TAU * uniform01(s), q[8], q[9]};
    }
    for (double& o : m_noise)
        o = std::floor(uniform01(s) * 4096.0) + uniform01(s);
    m_eventBase = splitmix(s) >> 44; // up to 2^20
    m_seedBase  = splitmix(s) >> 44;
}

CPatternLook::~CPatternLook() {
    release();
}

void CPatternLook::release() {
    for (auto& fb : m_fb)
        Gl::destroyFramebuffer(fb);
    for (auto& tex : m_chem)
        Gl::destroyTexture(tex);
    Gl::destroy(m_step);
    Gl::destroy(m_picture);
    m_health.release();
    m_w = m_h = 0;
}

bool CPatternLook::build(const SGeometry& g, float quality, std::string* error) {
    // the quality is the caller's (the picture's target): the lattice and the state stay
    if (m_w == g.cellsW && m_h == g.cellsH && m_step.id)
        return true;
    release();
    const auto fail = [&](std::string msg) {
        if (error)
            *error = std::move(msg);
        release();
        return false;
    };
    if (g.cellsW < 8 || g.cellsH < 8)
        return fail(std::format("a lattice of {}x{} cells", g.cellsW, g.cellsH));

    if (!m_fluid.build({g.cellsW, g.cellsH, false, OMEGA, PatternShaders::FORCE}, error))
        return fail(error ? *error : std::string{});
    const auto& fp = m_fluid.program();
    m_forceU       = {fp.uniform("stir"), fp.uniform("env"), fp.uniform("drag")};

    std::string log;
    auto        step = Gl::compile(PatternShaders::STEP, &log);
    if (!step)
        return fail(std::format("the chemistry shader failed to compile: {}", log));
    m_step  = *step;
    m_stepU = {m_step.uniform("chem"), m_step.uniform("vel"), m_step.uniform("size"), m_step.uniform("invShort"), m_step.uniform("n1i"),
               m_step.uniform("n1f"),  m_step.uniform("n2i"), m_step.uniform("n2f"),  m_step.uniform("seed")};
    auto picture = Gl::compile(std::string(GLSL_BILINEAR) + GLSL_BICUBIC + PatternShaders::PICTURE, &log);
    if (!picture)
        return fail(std::format("the picture shader failed to compile: {}", log));
    m_picture = *picture;
    m_picU    = {m_picture.uniform("chem"), m_picture.uniform("fade")};
    if (!m_health.build(HEALTH_CELL, {"chemTex", "fluidState"}, &log))
        return fail(log);

    for (int i = 0; i < 2; i++) {
        m_chem[i] = Gl::texture(GL_RGBA32F, g.cellsW, g.cellsH, GL_NEAREST);
        m_fb[i]   = Gl::framebuffer({m_chem[i]});
    }
    if (!m_fb[0] || !m_fb[1])
        return fail(std::format("the {}x{} RGBA32F chemistry framebuffers are incomplete", g.cellsW, g.cellsH));
    m_w        = g.cellsW;
    m_h        = g.cellsH;
    m_needInit = true;
    return true;
}

float CPatternLook::stepsPerSecond() const {
    return FLUID_SPS;
}

void CPatternLook::reset() {
    m_needInit = true;
    m_resets++;
    m_health.cancel(); // a verdict on the state before the reset is no longer true
    m_lastHealthy = true;
}

// The sketch's reset(): the fluid at rest, a = 1 everywhere and 60 spots of (0.5, 0.5),
// 9x9 cells each, at random places (as many per area on another lattice). Counters and
// events start again.
void CPatternLook::initialise() {
    m_fluid.init();
    uint64_t           s = m_seed ^ (0x51ed270b2f9e1d43ull * (m_resets + 1));
    const size_t       n = static_cast<size_t>(m_w) * m_h;
    std::vector<float> d(n * 4, 0.0f);
    for (size_t i = 0; i < n; i++)
        d[i * 4] = 1.0f;
    const int spots = std::max(1, static_cast<int>(std::lround(60.0 * m_w * m_h / (SKETCH_W * SKETCH_H))));
    for (int k = 0; k < spots; k++) {
        const double cx = uniform01(s) * m_w, cy = uniform01(s) * m_h;
        for (int y = -4; y <= 4; y++)
            for (int x = -4; x <= 4; x++) {
                const int X = static_cast<int>(std::floor(cx + x + m_w)) % m_w, Y = static_cast<int>(std::floor(cy + y + m_h)) % m_h;
                d[(static_cast<size_t>(Y) * m_w + X) * 4 + 0] = 0.5f;
                d[(static_cast<size_t>(Y) * m_w + X) * 4 + 1] = 0.5f;
            }
    }
    Gl::upload(m_chem[0], m_w, m_h, GL_RGBA, GL_FLOAT, d.data());
    m_cur        = 0;
    m_fsteps     = m_csteps = m_sinceInit = 0;
    m_events     = 0;
    m_spots      = 0;
    m_hasGust    = m_hasPending = false;
    m_lastKind   = -1;
    m_nextEvent  = static_cast<uint64_t>(FIRST_EVENT + EVENT_SPREAD * frac(static_cast<double>(m_eventBase) * PHI));
    m_needInit   = false;
    m_checkedAt   = ~uint64_t{0};
    m_lastHealthy = true;
}

// One event, the sketch's schedule: the next one 15000 to 30000 chemistry steps later by
// the golden ratio, positions by the plastic number and 0.5698, kinds in turn.
void CPatternLook::scheduleEvent() {
    m_events++;
    const double idx = static_cast<double>(m_eventBase + m_events);
    m_nextEvent      = m_csteps + static_cast<uint64_t>(EVENT_MIN + EVENT_SPREAD * frac(idx * PHI));
    const double ex = frac(idx * PLASTIC) * m_w, ey = frac(idx * THIRD) * m_h;
    m_lastKind      = static_cast<int>((m_eventBase + m_events) % 3);
    m_lastEventAt   = m_csteps;
    if (m_lastKind == 0) {
        m_pending    = {ex, ey, BURST_R, 1.0, BURST_N};
        m_hasPending = true;
    } else if (m_lastKind == 1) {
        m_pending    = {ex, ey, WIPE_R, 0.0, WIPE_N};
        m_hasPending = true;
    } else {
        const double a = idx * GOLDEN_ANGLE;
        m_gust         = {m_fsteps, ex, ey, GUST_SPEED * std::cos(a), GUST_SPEED * std::sin(a), GUST_STEPS};
        m_hasGust      = true;
    }
}

// A stirrer at fluid step t on a WxH lattice. The approved look is defined per cell:
// orbit amplitudes, speeds and the force radius are the sketch's, in cells, on every
// lattice; only the orbit centre is placed proportionally (the sketch's 400x225 layout
// spread over the lattice, its long axis along the lattice's long side), so the three
// stirrers share a wider or taller lattice. On 400x225 this is the sketch exactly.
CPatternLook::SStir CPatternLook::stirrer(size_t i, double t, int w, int h) const {
    const auto&  q        = m_stirrers[i];
    const bool   portrait = w < h;
    const double longS = (portrait ? h : w) / SKETCH_W, shortS = (portrait ? w : h) / SKETCH_H;
    const double X  = q.cx * longS + q.ax * std::sin(q.w1 * t + q.p1), Y = q.cy * shortS + q.ay * std::sin(q.w2 * t + q.p2);
    const double VX = q.ax * q.w1 * std::cos(q.w1 * t + q.p1), VY = q.ay * q.w2 * std::cos(q.w2 * t + q.p2);
    const double env = 1.6 * smoothstep(-0.2, 0.7, std::sin(q.e1 * t + q.p1) + 0.6 * std::sin(q.e2 * t + q.p2));
    return portrait ? SStir{Y, X, VY, VX, env} : SStir{X, Y, VX, VY, env};
}

void CPatternLook::fluidStep() {
    const double t = static_cast<double>(m_fsteps);
    GLfloat      stir[16] = {};
    GLfloat      env[4]   = {0, 0, 0, 0};
    for (size_t i = 0; i < m_stirrers.size(); i++) {
        const SStir s   = stirrer(i, t, m_w, m_h);
        stir[i * 4 + 0] = static_cast<GLfloat>(s.x);
        stir[i * 4 + 1] = static_cast<GLfloat>(s.y);
        stir[i * 4 + 2] = static_cast<GLfloat>(s.vx);
        stir[i * 4 + 3] = static_cast<GLfloat>(s.vy);
        env[i]          = static_cast<GLfloat>(s.env);
    }
    if (m_hasGust) {
        const double dt = static_cast<double>(m_fsteps - m_gust.t0);
        if (dt > m_gust.dur)
            m_hasGust = false;
        else {
            stir[12] = static_cast<GLfloat>(frac((m_gust.x + m_gust.vx * dt) / m_w) * m_w);
            stir[13] = static_cast<GLfloat>(frac((m_gust.y + m_gust.vy * dt) / m_h) * m_h);
            stir[14] = static_cast<GLfloat>(m_gust.vx);
            stir[15] = static_cast<GLfloat>(m_gust.vy);
            env[3]   = static_cast<GLfloat>(2.0 * smoothstep(0, 150, dt) * smoothstep(m_gust.dur, m_gust.dur - 150, dt));
        }
    }
    const auto& fp = m_fluid.program();
    Gl::use(fp);
    glUniform4fv(m_forceU.stir, 4, stir);
    glUniform4fv(m_forceU.env, 1, env);
    glUniform1f(m_forceU.drag, DRAG);
    m_fluid.step();
    m_fsteps++;
}

void CPatternLook::chemStep() {
    // sources: the small periodic seed, an event's disc (which takes over while it lasts)
    double sx = 0, sy = 0, sr = 0, sw = 1;
    if (m_csteps % SEED_EVERY == 0) {
        m_spots++;
        const double k = static_cast<double>(m_seedBase + m_spots);
        sx             = frac(k * PHI) * m_w;
        sy             = frac(k * PLASTIC) * m_h;
        sr             = SEED_R;
    }
    if (m_csteps >= m_nextEvent)
        scheduleEvent();
    if (m_hasPending) {
        sx = m_pending.x;
        sy = m_pending.y;
        sr = m_pending.r;
        sw = m_pending.w;
        if (--m_pending.n <= 0)
            m_hasPending = false;
    }

    // the feed/kill noise drift at this step, split into whole cells and the rest
    const double t     = static_cast<double>(m_csteps);
    const double o[4]  = {m_noise[0] + t * 2.1e-5, m_noise[1] + t * 1.3e-5, m_noise[2] - t * 1.1e-5, m_noise[3] + t * 2.7e-5};
    double       fl[4] = {};
    for (int i = 0; i < 4; i++)
        fl[i] = std::floor(o[i]);

    const int nxt = 1 - m_cur;
    Gl::use(m_step);
    Gl::bind(0, m_chem[m_cur]);
    Gl::bind(1, m_fluid.velocity());
    glUniform1i(m_stepU.chem, 0);
    glUniform1i(m_stepU.vel, 1);
    glUniform2i(m_stepU.size, m_w, m_h);
    glUniform1f(m_stepU.invShort, 1.0f / static_cast<float>(std::min(m_w, m_h)));
    glUniform2i(m_stepU.n1i, static_cast<GLint>(fl[0]), static_cast<GLint>(fl[1]));
    glUniform2f(m_stepU.n1f, static_cast<GLfloat>(o[0] - fl[0]), static_cast<GLfloat>(o[1] - fl[1]));
    glUniform2i(m_stepU.n2i, static_cast<GLint>(fl[2]), static_cast<GLint>(fl[3]));
    glUniform2f(m_stepU.n2f, static_cast<GLfloat>(o[2] - fl[2]), static_cast<GLfloat>(o[3] - fl[3]));
    glUniform4f(m_stepU.seed, static_cast<GLfloat>(sx), static_cast<GLfloat>(sy), static_cast<GLfloat>(sr), static_cast<GLfloat>(sw));
    Gl::pass(m_step, m_fb[nxt], 0, 0, m_w, m_h);
    m_cur = nxt;
    m_csteps++;
}

void CPatternLook::advance(int steps) {
    if (!m_w)
        return;
    if (m_needInit)
        initialise();
    // the sketch's frame: one fluid step, then the chemistry on the new velocity
    for (int i = 0; i < steps; i++) {
        fluidStep();
        for (int k = 0; k < CHEM_STEPS; k++)
            chemStep();
        m_sinceInit++;
    }
}

void CPatternLook::draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) {
    if (!m_w || !m_picture.id)
        return;
    Gl::use(m_picture);
    Gl::bind(0, m_chem[m_cur]);
    glUniform1i(m_picU.chem, 0);
    glUniform1f(m_picU.fade, std::clamp(fade, 0.0f, 1.0f));
    Gl::pass(m_picture, fbo, x, y, w, h, proj);
}

// One reduction per check, and none when nothing stepped since the last one (time_scale
// 0) or before the first step on a (re)initialised state: a non-finite value anywhere
// (a, b, glow, |u| or the density), a speed over the blow-up limit (.w is |u| of the
// last fluid step, the quantity CFluid::stats() judges) or a density at or below 0 means
// the fluid blew up; then the web's coverage limits.
void CPatternLook::checkHealth() {
    if (!m_w || m_needInit || m_sinceInit == 0 || m_sinceInit == m_checkedAt || m_health.pending())
        return;
    if (m_health.start({m_chem[m_cur], m_fluid.state()}, m_w, m_h))
        m_checkedAt = m_sinceInit;
}

EHealth CPatternLook::takeHealth(bool wait) {
    SReduced r;
    if (!m_health.take(r, wait))
        return wait ? (m_lastHealthy ? EHealth::HEALTHY : EHealth::BLOWN) : EHealth::NONE;
    const double n = static_cast<double>(m_w) * m_h;
    m_umax        = r.max[0];
    m_rhoMin      = -r.max[1];
    m_cover       = static_cast<float>(r.sum[0] / n);
    m_act         = static_cast<float>(r.sum[1] / n);
    m_lastHealthy = r.finite() && m_umax <= UMAX_BLOWN && m_rhoMin > 0.0f && m_cover >= COVER_DEAD && m_cover <= COVER_FULL;
    return m_lastHealthy ? EHealth::HEALTHY : EHealth::BLOWN;
}

std::string CPatternLook::status() const {
    const std::string last = m_lastKind < 0 ? std::string{"no event yet"} : std::format("last event {} at chemistry step {}", kindName(m_lastKind), m_lastEventAt);
    return std::format("pattern: lattice {}x{}, {} fluid steps ({} chemistry), web {:.1f}% of cells, glow {:.4f}, |u| max {:.4f}, rho min {:.4f}, {}, next in {} steps, {} resets",
                       m_w, m_h, m_fsteps, m_csteps, 100.0f * m_cover, m_act, m_umax, m_rhoMin, last, m_nextEvent > m_csteps ? m_nextEvent - m_csteps : 0, m_resets);
}
