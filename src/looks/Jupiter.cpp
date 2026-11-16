#include "looks/Jupiter.hpp"
#include "core/Compound.hpp"
#include "core/Sequence.hpp"
#include "core/Verdicts.hpp"
#include "looks/JupiterShaders.hpp"

#include <algorithm>
#include <cmath>
#include <format>

static_assert(Verdicts::JUPITER_CLOUD_PER_FRAME && Verdicts::JUPITER_CLOUD_R16F,
              "jupiter takes the accepted paths; a rejected one needs its own plugin path (only the bench builds the sketch's)");

namespace {

constexpr float    OMEGA = 1.88f;
constexpr double   ELIFE = 14000; // an eye's life in steps
constexpr float    UMAX_UNHEALTHY = 0.25f;

// The health check's reduction, per cell: |u| and the density's min and max; |u| and the
// density summed (the means); velocity and density must be finite.
constexpr const char* HEALTH_CELL = R"GLSL(
uniform sampler2D fluidState; // f8, ux, uy, rho
void cell(ivec2 c, out vec4 mx, out vec3 sm, out vec4 chk) {
    vec4  f = texelFetch(fluidState, c, 0);
    float u = length(f.yz);
    mx  = vec4(u, -f.w, f.w, 0.0);
    sm  = vec3(u, f.w, 0.0);
    chk = vec4(f.yzw, 0.0);
}
)GLSL";

} // namespace

// ── the schedule ────────────────────────────────────────────────────────────────────

int CJupiterSchedule::jetsFor(int cellsH) {
    if (cellsH == 225)
        return 7;
    // the odd number nearest 7 H / 225 (at least 1)
    const double x = 7.0 * cellsH / 225.0;
    const int    k = static_cast<int>(std::lround((x - 1.0) / 2.0));
    return std::max(1, 2 * k + 1);
}

CJupiterSchedule::CJupiterSchedule(int cellsW, int cellsH, uint64_t seed) : m_w(cellsW), m_h(cellsH), m_n(jetsFor(cellsH)) {
    reseed(seed);
}

void CJupiterSchedule::reseed(uint64_t seed) {
    m_stormBase = m_eyeBase = 0; // seed 0: the sketch's own sequence
    if (seed != 0) {
        uint64_t s  = seed ^ 0x6a75706974ull; // "jupit": not another look's sequence for the same monitor
        m_stormBase = splitmix(s) >> 44;      // up to 2^20, as the other looks'
        m_eyeBase   = splitmix(s) >> 44;
    }
    restart();
}

void CJupiterSchedule::restart() {
    m_at = m_ev = m_en = m_lastOutbreak = 0;
    m_next = 500;
    m_e0   = 2500;
    m_ex = m_ey = 0;
    m_storms.clear();
    m_active.clear();
    m_eye = {0, 0, 0, 0};
}

void CJupiterSchedule::advanceTo(uint64_t step) {
    while (m_at <= step)
        stepOnce(m_at++);
}

// sketchJove's frame loop for one step, events only (reference/b7-sketch.html), with n
// jets in place of 7 and the counters from their bases.
void CJupiterSchedule::stepOnce(uint64_t s) {
    const double step = static_cast<double>(s), W = m_w, H = m_h;
    if (step >= m_next) {
        m_ev++;
        m_lastOutbreak = s;
        const double i = static_cast<double>(m_stormBase + m_ev);
        m_next         = step + 6500 + 5000 * frac(i * PHI);
        const double edge = ((std::floor(frac(i * PLASTIC) * (m_n - 1)) + 1) / m_n) * H, x0 = frac(i * PHI) * W;
        const int    cnt  = 3 + static_cast<int>(std::fmod(i, 2.0));
        m_storms.clear();
        for (int k = 0; k < cnt; k++) {
            const double h = frac((i * 7 + k * 3) * 0.5698402910);
            m_storms.push_back({std::fmod(x0 + k * (55 + 30 * h), W), edge + (h - 0.5) * 34, 11 + 12 * h, (k % 2 ? -1.0 : 1.0) * (0.7 + 0.6 * h),
                                frac(k * 0.37 + i * 0.21) > 0.5 ? 0.5 : (k % 2 ? 0.0 : 0.72), step + k * 260, 1500 + 900 * h, edge});
        }
    }
    if (step == m_e0) {
        m_en++;
        const double j = static_cast<double>(m_eyeBase + m_en);
        // the sketch's [1, 5, 3][en % 3] / 7: the odd edges 1, n - 2, ..., 3 in turn
        const int m  = std::max(1, (m_n - 1) / 2);
        const int jm = static_cast<int>(std::fmod(j, static_cast<double>(m)));
        m_ey         = (2 * ((m - jm) % m) + 1) / static_cast<double>(m_n) * H;
        m_ex         = frac(j * PLASTIC + 0.3) * W;
    }
    const double dt = step - m_e0;
    if (dt >= 0 && dt <= ELIFE)
        m_eye = {std::fmod(m_ex - 0.004 * dt + 4 * W, W), m_ey, 20, smoothstep(0, 1500, dt) * smoothstep(ELIFE, ELIFE - 2500, dt)};
    else {
        m_eye.envelope = 0;
        if (dt > ELIFE)
            m_e0 = step + 9000 + std::floor(8000 * frac(static_cast<double>(m_eyeBase + m_en) * PHI));
    }
    m_active.clear();
    for (size_t k = 0; k < m_storms.size(); k++) {
        const auto&  q  = m_storms[k];
        const double dq = step - q.t0;
        if (dq < 0 || dq > q.dur)
            continue;
        const double e = smoothstep(0, 250, dq) * smoothstep(q.dur, q.dur - 500, dq);
        m_active.push_back({q.x, q.y, q.r, q.s * e, q.c, static_cast<uint64_t>(q.t0), q.dur, static_cast<int>(k), q.edge});
    }
}

// ── the look ────────────────────────────────────────────────────────────────────────

CJupiterLook::CJupiterLook(uint64_t seed, bool sketchPath) : m_seed(seed), m_sketch(sketchPath), m_sched(400, 225, seed) {}

CJupiterLook::~CJupiterLook() {
    release();
}

double CJupiterLook::stepFactor(double a, int n) {
    return n == 1 ? a : compound(a, n);
}

uint64_t CJupiterLook::grainTick(uint64_t ns) {
    return static_cast<uint64_t>(static_cast<unsigned __int128>(ns) * 24u / 1'000'000'000u);
}

uint32_t CJupiterLook::grainFrame(uint64_t tick) {
    uint64_t s = tick;
    return static_cast<uint32_t>(splitmix(s) & 4095u);
}

std::array<int, 2> CJupiterLook::cloudSize(int w, int h, float quality) {
    return {std::max(1, static_cast<int>(std::lround(static_cast<double>(K) * w * quality))), std::max(1, static_cast<int>(std::lround(static_cast<double>(K) * h * quality)))};
}

void CJupiterLook::releaseCloud() {
    for (int i = 0; i < 2; i++) {
        Gl::destroyFramebuffer(m_cloudFb[i]);
        Gl::destroyTexture(m_cloud[i]);
    }
    Gl::destroyFramebuffer(m_hatFb);
    Gl::destroyFramebuffer(m_barFb);
    Gl::destroyTexture(m_hat);
    Gl::destroyTexture(m_bar);
    m_cw = m_ch = 0;
    m_cur       = 0;
}

void CJupiterLook::release() {
    releaseCloud();
    Gl::destroy(m_adv);
    Gl::destroy(m_mac);
    Gl::destroy(m_seedProg);
    Gl::destroy(m_resample);
    Gl::destroy(m_picture);
    m_health.release();
    m_w = m_h = m_pw = m_ph = 0;
    m_seeded  = false;
}

// The cloud pair and the two temporaries at cw x ch: R16F (R32F on the sketch's path),
// linear, x repeating, y clamped. A cloud there before is resampled into the new one by
// one linear pass (a quality change); the old textures go after it.
bool CJupiterLook::makeCloud(int cw, int ch, std::string* error) {
    const GLenum fmt = m_sketch ? GL_R32F : GL_R16F;
    GLuint       tex[4] = {}, fb[4] = {};
    for (int i = 0; i < 4; i++) {
        tex[i] = Gl::texture(fmt, cw, ch, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE);
        fb[i]  = Gl::framebuffer({tex[i]});
        if (!fb[i]) {
            for (int k = 0; k <= i; k++) {
                Gl::destroyFramebuffer(fb[k]);
                Gl::destroyTexture(tex[k]);
            }
            if (error)
                *error = std::format("the jupiter cloud's {}x{} {} framebuffer is incomplete", cw, ch, m_sketch ? "R32F" : "R16F");
            return false;
        }
    }
    const bool carry = m_seeded && m_cloud[m_cur];
    if (carry) {
        Gl::use(m_resample);
        Gl::bind(0, m_cloud[m_cur]);
        glUniform2i(m_cloudU[3].size, cw, ch);
        Gl::pass(m_resample, fb[0], 0, 0, cw, ch);
        m_resampled = std::format("cloud {}x{} resampled into {}x{} (one linear pass)", m_cw, m_ch, cw, ch);
    }
    releaseCloud();
    m_cloud[0] = tex[0], m_cloudFb[0] = fb[0];
    m_cloud[1] = tex[1], m_cloudFb[1] = fb[1];
    m_hat = tex[2], m_hatFb = fb[2];
    m_bar = tex[3], m_barFb = fb[3];
    m_cw = cw, m_ch = ch;
    m_cur    = 0;
    m_seeded = carry;
    // K per axis on every cloud program
    const GLfloat kx = static_cast<GLfloat>(static_cast<double>(cw) / m_w), ky = static_cast<GLfloat>(static_cast<double>(ch) / m_h);
    const Gl::SProgram* progs[4] = {&m_adv, &m_mac, &m_seedProg, &m_resample};
    for (int i = 0; i < 4; i++) {
        Gl::use(*progs[i]);
        glUniform2i(m_cloudU[i].size, cw, ch);
        glUniform2i(m_cloudU[i].lsize, m_w, m_h);
        glUniform2f(m_cloudU[i].k, kx, ky);
    }
    return true;
}

bool CJupiterLook::build(const SGeometry& g, float quality, std::string* error) {
    const int  pw = Gl::scaledSide(g.monW, quality), ph = Gl::scaledSide(g.monH, quality);
    const auto fail = [&](std::string msg) {
        if (error)
            *error = std::move(msg);
        release();
        return false;
    };
    m_monW = g.monW;
    m_monH = g.monH;
    if (m_w == g.cellsW && m_h == g.cellsH && m_picture.id) {
        // the lattice stays with its state; the cloud follows the quality, carried over
        const auto cs = cloudSize(m_w, m_h, quality);
        m_pw          = pw;
        m_ph          = ph;
        if (cs[0] == m_cw && cs[1] == m_ch)
            return true;
        std::string err;
        if (!makeCloud(cs[0], cs[1], &err))
            return fail(err);
        if (!m_seeded)
            m_needInit = true;
        return true;
    }
    release();
    if (g.cellsW < 16 || g.cellsH < 16 || pw < 1 || ph < 1)
        return fail(std::format("a lattice of {}x{} cells, a picture of {}x{}", g.cellsW, g.cellsH, pw, ph));
    if (m_sketch && !Gl::hasExtension("GL_OES_texture_float_linear"))
        return fail("the sketch's path filters an R32F cloud: GL_OES_texture_float_linear is missing");
    const int W = g.cellsW, H = g.cellsH;
    m_w = W;
    m_h = H;
    m_sched = CJupiterSchedule(W, H, m_seed);

    if (!m_fluid.build({W, H, true, OMEGA, JupiterShaders::FORCE}, error))
        return fail(error ? *error : std::string{});
    const auto& fp = m_fluid.program();
    m_forceU       = {fp.uniform("st"), fp.uniform("eye"), fp.uniform("jets")};
    Gl::use(fp);
    glUniform1f(m_forceU.jets, static_cast<GLfloat>(m_sched.jets()));

    const std::string head = std::format("#define PER_FRAME {}\n#define R16 {}\n", m_sketch ? 0 : 1, m_sketch ? 0 : 1) + JupiterShaders::SROUND16 + JupiterShaders::CLOUD_HEAD;
    std::string       log;
    const auto        make = [&](Gl::SProgram& out, const std::string& src) {
        auto p = Gl::compile(src, &log);
        if (!p)
            return false;
        out = *p;
        return true;
    };
    if (!make(m_adv, head + JupiterShaders::ADVECT))
        return fail(std::format("the jupiter advection shader failed to compile: {}", log));
    if (!make(m_mac, head + JupiterShaders::MACCORMACK))
        return fail(std::format("the jupiter MacCormack shader failed to compile: {}", log));
    if (!make(m_seedProg, head + JupiterShaders::SEED))
        return fail(std::format("the jupiter seed shader failed to compile: {}", log));
    if (!make(m_resample, head + JupiterShaders::RESAMPLE))
        return fail(std::format("the jupiter resample shader failed to compile: {}", log));
    if (!make(m_picture, JupiterShaders::PICTURE))
        return fail(std::format("the jupiter picture shader failed to compile: {}", log));
    if (!m_health.build(HEALTH_CELL, {"fluidState"}, &log))
        return fail(log);

    // samplers: S 0, V 1, HAT 2, BAR 3; the jets on every program that draws bands
    const GLfloat jets = static_cast<GLfloat>(m_sched.jets()), fine = static_cast<GLfloat>(17.0 * m_sched.jets() / 7.0);
    for (const Gl::SProgram* p : {&m_adv, &m_mac, &m_seedProg, &m_resample, &m_picture}) {
        Gl::use(*p);
        glUniform1i(p->uniform("S"), 0);
        glUniform1i(p->uniform("V"), 1);
        glUniform1i(p->uniform("HAT"), 2);
        glUniform1i(p->uniform("BAR"), 3);
        glUniform1f(p->uniform("jets"), jets);
        glUniform1f(p->uniform("fine"), fine);
    }
    const Gl::SProgram* cloudProgs[4] = {&m_adv, &m_mac, &m_seedProg, &m_resample};
    for (int i = 0; i < 4; i++)
        m_cloudU[i] = {cloudProgs[i]->uniform("size"), cloudProgs[i]->uniform("lsize"), cloudProgs[i]->uniform("K")};
    m_advU = {m_adv.uniform("dir"), m_adv.uniform("frame")};
    m_macU = {m_mac.uniform("st"), m_mac.uniform("sc"), m_mac.uniform("backN"), m_mac.uniform("eye"), m_mac.uniform("nst"), m_mac.uniform("frame")};
    m_picU = {m_picture.uniform("size"), m_picture.uniform("view"), m_picture.uniform("mon"), m_picture.uniform("grain"), m_picture.uniform("fade")};

    const auto  cs = cloudSize(W, H, quality);
    std::string err;
    if (!makeCloud(cs[0], cs[1], &err))
        return fail(err);
    m_pw       = pw;
    m_ph       = ph;
    m_needInit = true;
    return true;
}

float CJupiterLook::stepsPerSecond() const {
    return SPS;
}

SLookInfo CJupiterLook::info() const {
    return {.wallClock = true}; // the grain (setWallClock)
}

void CJupiterLook::setFrameRate(float fps) {
    if (fps > 0)
        m_fps = fps; // nothing here depends on it (no bake); kept for the status
}

void CJupiterLook::setWallClock(uint64_t ns) {
    m_wallNs = ns;
}

void CJupiterLook::reset() {
    m_needInit = true;
    m_resets++;
    m_health.cancel(); // a verdict on the state before the reset is no longer true
    m_lastHealthy = true;
}

void CJupiterLook::initialise() {
    m_fluid.init();
    Gl::use(m_seedProg);
    Gl::pass(m_seedProg, m_cloudFb[0], 0, 0, m_cw, m_ch);
    m_cur    = 0;
    m_seeded = true;
    // After a reset (unhealthy, or `hyprctl hyprns reset`) the events come from a new seed:
    // the fluid restarts from rest and the GPU path is deterministic, so the same events
    // would replay whatever led to the reset. The first initialisation keeps the seed.
    if (m_resets == 0)
        m_sched.reseed(m_seed);
    else {
        uint64_t s = m_seed ^ m_resets;
        m_sched.reseed(splitmix(s));
    }
    std::fill(std::begin(m_sc), std::end(m_sc), 0.0f);
    m_steps = m_sinceInit = m_transports = 0;
    m_umax = m_umean = m_umaxLate = 0;
    m_rhoMin = m_rhoMinLate = m_rhoMax = m_rhoMean = 1; // the rest state's density, until a check sees otherwise
    m_checkedAt      = ~uint64_t{0};
    m_lastHealthy    = true;
    m_needInit       = false;
}

// the sketch's sc: a slot keeps the colour of the last storm alive in it
void CJupiterLook::syncColours() {
    for (const auto& q : m_sched.storms())
        m_sc[q.slot] = static_cast<float>(q.colour);
}

// the sketch's st[4]: per slot (x, y, r, strength x envelope), r 0 for a slot not alive
void CJupiterLook::packStorms(GLfloat st[16]) const {
    std::fill(st, st + 16, 0.0f);
    for (const auto& q : m_sched.storms()) {
        st[q.slot * 4 + 0] = static_cast<GLfloat>(q.x);
        st[q.slot * 4 + 1] = static_cast<GLfloat>(q.y);
        st[q.slot * 4 + 2] = static_cast<GLfloat>(q.r);
        st[q.slot * 4 + 3] = static_cast<GLfloat>(q.strength);
    }
}

// The storms and the eye of this step (m_sched at m_steps) on the force.
void CJupiterLook::setForce() {
    GLfloat st[16];
    packStorms(st);
    const SEye e = m_sched.eye();
    Gl::use(m_fluid.program());
    glUniform4fv(m_forceU.st, 4, st);
    glUniform4f(m_forceU.eye, static_cast<GLfloat>(e.x), static_cast<GLfloat>(e.y), static_cast<GLfloat>(e.r), static_cast<GLfloat>(e.envelope));
}

// The cloud moved by n steps on the current velocity (n 1: the sketch's step), with the
// sources of the current schedule step: ADV forward, ADV backward, MacCormack + sources.
void CJupiterLook::transport(int n) {
    GLfloat st[16];
    packStorms(st);
    const SEye    e     = m_sched.eye();
    const GLfloat frame = static_cast<GLfloat>(m_transports % 4096);
    const GLfloat fn    = static_cast<GLfloat>(n);

    Gl::use(m_adv);
    Gl::bind(1, m_fluid.velocity());
    glUniform1f(m_advU.frame, frame);
    Gl::bind(0, m_cloud[m_cur]);
    glUniform1f(m_advU.dir, fn);
    Gl::pass(m_adv, m_hatFb, 0, 0, m_cw, m_ch);
    Gl::bind(0, m_hat);
    glUniform1f(m_advU.dir, -fn);
    Gl::pass(m_adv, m_barFb, 0, 0, m_cw, m_ch);

    Gl::use(m_mac);
    Gl::bind(0, m_cloud[m_cur]);
    Gl::bind(1, m_fluid.velocity());
    Gl::bind(2, m_hat);
    Gl::bind(3, m_bar);
    glUniform4fv(m_macU.st, 4, st);
    glUniform4fv(m_macU.sc, 1, m_sc);
    glUniform1f(m_macU.backN, static_cast<GLfloat>(backN(n)));
    glUniform4f(m_macU.eye, static_cast<GLfloat>(e.x), static_cast<GLfloat>(e.y), static_cast<GLfloat>(e.r), static_cast<GLfloat>(e.envelope));
    glUniform1f(m_macU.nst, fn);
    glUniform1f(m_macU.frame, frame);
    Gl::pass(m_mac, m_cloudFb[1 - m_cur], 0, 0, m_cw, m_ch);
    m_cur = 1 - m_cur;
    m_transports++;
}

void CJupiterLook::advance(int steps) {
    if (!m_w)
        return;
    if (m_needInit)
        initialise();
    if (steps <= 0)
        return;
    if (m_sketch) {
        // the sketch: the cloud after every step, with that step's sources
        for (int i = 0; i < steps; i++) {
            m_sched.advanceTo(m_steps);
            syncColours();
            setForce();
            m_fluid.step();
            transport(1);
            m_steps++;
        }
    } else {
        // jupiter-cloud-per-frame: once per transport of at most 64 steps, on the last
        // velocity, with the last step's storms and eye
        for (int left = steps; left > 0;) {
            const int n = std::min(left, MAX_TRANSPORT_STEPS);
            for (int k = 0; k < n; k++) {
                m_sched.advanceTo(m_steps);
                syncColours();
                setForce();
                m_fluid.step();
                m_steps++;
            }
            transport(n);
            left -= n;
        }
    }
    m_sinceInit += static_cast<uint64_t>(steps);
}

void CJupiterLook::draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) {
    if (!m_w || !m_picture.id || !m_cloud[m_cur])
        return;
    Gl::use(m_picture);
    Gl::bind(0, m_cloud[m_cur]);
    glUniform2i(m_picU.size, m_cw, m_ch);
    glUniform2f(m_picU.view, static_cast<float>(m_pw), static_cast<float>(m_ph));
    glUniform2f(m_picU.mon, static_cast<float>(m_monW), static_cast<float>(m_monH));
    glUniform1f(m_picU.grain, static_cast<float>(grainFrame(grainTick(m_wallNs))));
    // black until the cloud has been written once (a fresh build draws before its first advance)
    glUniform1f(m_picU.fade, m_seeded ? std::clamp(fade, 0.0f, 1.0f) : 0.0f);
    Gl::pass(m_picture, fbo, x, y, w, h, proj);
}

// One reduction of the fluid per check (CFluid::stats()'s quantities on the GPU), none
// when nothing stepped since.
void CJupiterLook::checkHealth() {
    if (!m_w || m_needInit || m_sinceInit == 0 || m_sinceInit == m_checkedAt || m_health.pending())
        return;
    if (m_health.start({m_fluid.state()}, m_w, m_h))
        m_checkedAt = m_sinceInit;
}

EHealth CJupiterLook::takeHealth(bool wait) {
    SReduced r;
    if (!m_health.take(r, wait))
        return wait ? (m_lastHealthy ? EHealth::HEALTHY : EHealth::BLOWN) : EHealth::NONE;
    const double n = static_cast<double>(m_w) * m_h;
    m_umax         = r.max[0];
    m_rhoMin       = -r.max[1];
    m_rhoMax       = r.max[2];
    m_umean        = static_cast<float>(r.sum[0] / n);
    m_rhoMean      = static_cast<float>(r.sum[1] / n);
    if (m_checkedAt > 3000) {
        m_umaxLate   = std::max(m_umaxLate, m_umax);
        m_rhoMinLate = std::min(m_rhoMinLate, m_rhoMin);
    }
    m_span.checks++;
    m_span.umax       = std::max(m_span.umax, m_umax);
    m_span.rhoMin     = std::min(m_span.rhoMin, m_rhoMin);
    m_span.rhoMax     = std::max(m_span.rhoMax, m_rhoMax);
    m_span.rhoMeanMin = std::min(m_span.rhoMeanMin, m_rhoMean);
    m_span.rhoMeanMax = std::max(m_span.rhoMeanMax, m_rhoMean);
    m_lastHealthy     = r.finite() && m_umax <= UMAX_UNHEALTHY && m_rhoMin > 0.0f;
    return m_lastHealthy ? EHealth::HEALTHY : EHealth::BLOWN;
}

CJupiterLook::SSpan CJupiterLook::takeSpan() {
    const SSpan s = m_span;
    m_span        = {};
    return s;
}

std::string CJupiterLook::status() const {
    const std::string path = m_sketch ? std::string{"path sketch (cloud per step, R32F; forced, bench only)"}
                                      : std::string{"path accepted (jupiter-cloud-per-frame, jupiter-cloud-r16f)"};
    const SEye        e    = m_sched.eye();
    return std::format("jupiter: lattice {}x{}, {} jets, cloud {}x{}, picture {}x{}, {} steps, {}, {} storms alive ({} outbreaks, last at step {}), eye {} (envelope {:.2f}, "
                       "{} eyes), |u| max {:.4f} mean {:.5f} (largest after step 3000 {:.4f}), rho min {:.4f} (smallest after step 3000 {:.4f}) max {:.4f} mean {:.5f}, {} resets, grain on wall time (renews at time_scale 0){}",
                       m_w, m_h, m_sched.jets(), m_cw, m_ch, m_pw, m_ph, m_steps, path, m_sched.storms().size(), m_sched.outbreaks(), m_sched.lastOutbreakAt(),
                       e.envelope > 0 ? std::format("up at ({:.0f}, {:.0f})", e.x, e.y) : std::string{"down"}, e.envelope, m_sched.eyes(), m_umax, m_umean, m_umaxLate, m_rhoMin, m_rhoMinLate, m_rhoMax, m_rhoMean, m_resets,
                       m_resampled.empty() ? std::string{} : ", " + m_resampled);
}
