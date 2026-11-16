#include "looks/Embers.hpp"
#include "core/Compound.hpp"
#include "core/Sequence.hpp"
#include "core/Verdicts.hpp"
#include "looks/EmbersShaders.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>
#include <vector>

static_assert(Verdicts::EMBERS_BAKE && Verdicts::EMBERS_PROXIMITY_BLUR && Verdicts::EMBERS_HEAT_PER_FRAME,
              "embers takes the accepted paths; a rejected one needs its own plugin path (only the bench builds the sketch's)");

namespace {

constexpr double SKETCH_W = 400.0, SKETCH_H = 225.0;
constexpr double TAU      = 6.283185307179586;

// The page's FIRE_ST (the same table as marble's and pattern's): centre, amplitude,
// angular speeds, phases, the unevenness' speeds, per step.
constexpr std::array<std::array<double, 10>, 3> STIRRERS = {{
    {100, 112, 120, 70, 6.2e-4, 6.2e-4 * 1.6180339887, 0.0, 1.3, 1.8e-3, 1.8e-3 * 0.6180339887},
    {300, 112, 110, 80, 5.2e-4 * 1.4142135623, 5.2e-4, 2.1, 0.4, 1.5e-3, 1.5e-3 * 1.4142135623},
    {200, 60, 160, 50, 4.4e-4, 4.4e-4 * 2.2360679775, 4.0, 3.3, 2.1e-3, 2.1e-3 * 0.7548776662},
}};

constexpr float OMEGA = 1.9f;

// the coals creep (CREEP) and the pulse's rate, per step
constexpr double CREEP = 6.5e-4, PULSE = 2e-3;

// The pops and gusts (the page's frame loop)
constexpr double FIRST_POP = 2500, FIRST_GUST = 7000, POP_HEAT = 1.7;
constexpr int    POP_STEPS = 30;
constexpr double GUST_STEPS = 1800;

// Health: a fluid speed no stable run reaches (CFluid::stats()'s limit); more heat than
// any source sets (the pop's 1.7); a bed with no cell over 0.05 after 960 steps (two
// seconds) is dead (its sources hold some cells at 0.35 x the bed's heat or more).
constexpr float    UMAX_BLOWN = 0.4f, TMAX_RUNAWAY = 1.75f, TMIN_ALIVE = 0.05f;
constexpr uint64_t ALIVE_AFTER = 960;

// The health check's reduction, per cell: |u|, the heat, the density's min and max; the
// density summed; heat, |u| and the density must be finite.
constexpr const char* HEALTH_CELL = R"GLSL(
uniform sampler2D heatTex;    // heat, |u|
uniform sampler2D fluidState; // f8, ux, uy, rho
void cell(ivec2 c, out vec4 mx, out vec3 sm, out vec4 chk) {
    vec2  h   = texelFetch(heatTex, c, 0).xy;
    float rho = texelFetch(fluidState, c, 0).w;
    mx  = vec4(h.y, h.x, -rho, rho);
    sm  = vec3(rho, 0.0, 0.0);
    chk = vec4(h, rho, 0.0);
}
)GLSL";

constexpr GLenum BAKE_FORMAT = GL_RGBA8;

// the blur's sigma: the second moment of the sketch's 32-tap ring (ringSigma(14))
double ringSigma(double reach) {
    double sw = 0, sr = 0;
    for (int k = 0; k < 32; k++) {
        const double r = reach * (k / 8 + 1) / 4.0, w = std::exp(-2 * r * r / (reach * reach));
        sw += w;
        sr += w * r * r;
    }
    return std::sqrt(sr / sw / 2);
}

double modPos(double x, double p) {
    const double r = x - p * std::floor(x / p);
    return r >= p || r < 0 ? 0.0 : r;
}

float modPosF(double x, double p) {
    const float r = static_cast<float>(modPos(x, p));
    return r >= static_cast<float>(p) ? 0.0f : r;
}

} // namespace

void CEmbersEvents::restart() {
    m_ev = m_pops = m_gusts = m_lastPopAt = m_gustT0 = 0;
    m_nextPop   = FIRST_POP;
    m_nextGust  = FIRST_GUST;
    m_popAlive  = m_gustAlive = false;
    m_popLeft   = 0;
}

CEmbersEvents::SStep CEmbersEvents::step(uint64_t s, int w, int h) {
    const bool   portrait = w < h;
    const double longS = portrait ? h : w, shortS = portrait ? w : h;
    const double t  = static_cast<double>(s);
    double       wx = 2.2e-6 * (std::sin(t * 7.1e-4) + 0.7 * std::sin(t * 7.1e-4 * 1.6180339887 + 1.0)) * smoothstep(0.2, 0.9, std::abs(std::sin(t * 2.3e-4 * 1.4142135623)));
    double       wy = 0;
    if (t >= m_nextPop) {
        m_ev++;
        m_pops++;
        const double i = static_cast<double>(m_base + m_ev);
        m_nextPop      = t + 3000 + 6000 * frac(i * PHI);
        m_px           = frac(i * PLASTIC) * longS;
        m_py           = frac(i * 0.5698402910) * shortS;
        m_pr           = 7 + 6 * frac(i * 0.3819660113);
        m_popLeft      = POP_STEPS;
        m_popAlive     = true;
        m_lastPopAt    = s;
    }
    if (t >= m_nextGust) {
        m_ev++;
        m_gusts++;
        const double i = static_cast<double>(m_base + m_ev);
        m_nextGust     = t + 12000 + 13000 * frac(i * PHI);
        const double a = i * 2.39996;
        m_gustT0       = s;
        m_gcx          = std::cos(a);
        m_gcy          = std::sin(a);
        m_gustAlive    = true;
    }
    if (m_gustAlive) {
        const double dt = t - static_cast<double>(m_gustT0);
        if (dt > GUST_STEPS)
            m_gustAlive = false;
        else {
            const double g = 9e-6 * smoothstep(0, 300, dt) * smoothstep(GUST_STEPS, GUST_STEPS - 500, dt);
            wx += m_gcx * g;
            wy += m_gcy * g;
        }
    }
    SStep o;
    o.wx = portrait ? wy : wx;
    o.wy = portrait ? wx : wy;
    if (m_popAlive) {
        o.pop     = true;
        o.popX    = portrait ? m_py : m_px;
        o.popY    = portrait ? m_px : m_py;
        o.popR    = m_pr;
        o.popHeat = POP_HEAT;
        if (--m_popLeft <= 0)
            m_popAlive = false;
    }
    return o;
}

CEmbersLook::CEmbersLook(uint64_t seed, bool sketchPath) : m_seed(seed), m_sketch(sketchPath), m_events(0) {
    uint64_t s = seed ^ 0x656d627231ull; // "embr1": not marble's or pattern's sequence for the same monitor
    for (size_t i = 0; i < m_stirrers.size(); i++) {
        const auto& q = STIRRERS[i];
        m_stirrers[i] = {q[0], q[1], q[2], q[3], q[4], q[5], q[6] + TAU * uniform01(s), q[7] + TAU * uniform01(s), q[8], q[9]};
    }
    m_hashBase[0] = static_cast<int>(uniform01(s) * 4096.0);
    m_hashBase[1] = static_cast<int>(uniform01(s) * 4096.0);
    m_eventBase   = splitmix(s) >> 44; // up to 2^20, as pattern's
    m_events      = CEmbersEvents(m_eventBase);
}

CEmbersLook::~CEmbersLook() {
    release();
}

void CEmbersLook::release() {
    m_bake.release();
    for (int i = 0; i < 2; i++) {
        Gl::destroyFramebuffer(m_blurFb[i]);
        Gl::destroyTexture(m_blurTex[i]);
    }
    Gl::destroy(m_blurProg);
    Gl::destroy(m_bakeProg);
    Gl::destroy(m_picture);
    m_health.release();
    m_w = m_h = m_pw = m_ph = 0;
}

CEmbersLook::SHeatFactors CEmbersLook::heatFactors(int n) {
    const double rN = std::pow(1.0 - C1, n);
    return {compound(4.0 * KAPPA, n), rN, C2 * (1.0 - rN) / C1};
}

double CEmbersLook::decayN(double f, int m) {
    const double r = std::pow(1.0 - C1, m);
    return std::max(f * r - C2 * (1.0 - r) / C1, 0.0);
}

CEmbersLook::SPeriods CEmbersLook::bedPeriods(int w, int h) {
    const bool   portrait = w < h;
    const double L = portrait ? h : w, S = portrait ? w : h;
    const auto   per = [](double cells, double sketchPeriods, double sketchCells) { return std::max(1, static_cast<int>(std::lround(sketchPeriods * cells / sketchCells))); };
    const int    b1L = per(L, 10, SKETCH_W), b1S = per(S, 6, SKETCH_H), b2L = per(L, 16, SKETCH_W), b2S = per(S, 9, SKETCH_H);
    SPeriods     p{};
    p.bed1[0]  = portrait ? b1S : b1L;
    p.bed1[1]  = portrait ? b1L : b1S;
    p.bed2[0]  = portrait ? b2S : b2L;
    p.bed2[1]  = portrait ? b2L : b2S;
    p.flick[0] = std::max(1, static_cast<int>(std::lround(w * 0.08)));
    p.flick[1] = std::max(1, static_cast<int>(std::lround(h * 0.08)));
    return p;
}

CEmbersLook::SNoiseTime CEmbersLook::noiseAt(double step, int w, int h) {
    const bool     portrait = w < h;
    const SPeriods p        = bedPeriods(w, h);
    const double   t        = step * SHIFT;
    // the sketch's frame: layer 1 added, layer 2 subtracted, the flicker along y
    double d1[2] = {t * 3e-5, t * 1.1e-5}, d2[2] = {t * 2e-5, -t * 2.6e-5};
    if (portrait) {
        std::swap(d1[0], d1[1]);
        std::swap(d2[0], d2[1]);
    }
    SNoiseTime o{};
    for (int a = 0; a < 2; a++) {
        o.bed1[a] = modPosF(d1[a], p.bed1[a]);
        o.bed2[a] = modPosF(d2[a], p.bed2[a]);
    }
    o.flick = modPosF(step * 4e-3, portrait ? p.flick[0] : p.flick[1]);
    return o;
}

// As pattern's and marble's: amplitudes, speeds and the force radius in cells on every
// lattice, the orbit centre placed proportionally (the sketch's long axis along the long side).
CEmbersLook::SStir CEmbersLook::stirrer(size_t i, double t, int w, int h) const {
    const auto&  q        = m_stirrers[i];
    const bool   portrait = w < h;
    const double longS = (portrait ? h : w) / SKETCH_W, shortS = (portrait ? w : h) / SKETCH_H;
    const double X  = q.cx * longS + q.ax * std::sin(q.w1 * t + q.p1), Y = q.cy * shortS + q.ay * std::sin(q.w2 * t + q.p2);
    const double VX = q.ax * q.w1 * std::cos(q.w1 * t + q.p1), VY = q.ay * q.w2 * std::cos(q.w2 * t + q.p2);
    const double env = 1.6 * smoothstep(-0.2, 0.7, std::sin(q.e1 * t + q.p1) + 0.6 * std::sin(q.e2 * t + q.p2));
    return portrait ? SStir{Y, X, VY, VX, env} : SStir{X, Y, VX, VY, env};
}

bool CEmbersLook::build(const SGeometry& g, float quality, std::string* error) {
    const int  pw = Gl::scaledSide(g.monW, quality), ph = Gl::scaledSide(g.monH, quality);
    const auto fail = [&](std::string msg) {
        if (error)
            *error = std::move(msg);
        release();
        return false;
    };
    if (m_w == g.cellsW && m_h == g.cellsH && m_picture.id) {
        // the lattice stays, with its state: only the picture's size (the bake) changes,
        // the shown keys stay on screen while the new ones prime
        if (pw == m_pw && ph == m_ph)
            return true;
        m_pw = pw;
        m_ph = ph;
        std::string err;
        if (!m_sketch && !buildBake(true, &err))
            return fail(err);
        return true;
    }
    release();
    if (g.cellsW < 16 || g.cellsH < 16 || pw < 1 || ph < 1)
        return fail(std::format("a lattice of {}x{} cells, a picture of {}x{}", g.cellsW, g.cellsH, pw, ph));
    const int W = g.cellsW, H = g.cellsH;

    if (!m_fluid.build({W, H, false, OMEGA, EmbersShaders::FORCE}, error))
        return fail(error ? *error : std::string{});
    const auto& fp = m_fluid.program();
    m_forceU       = {fp.uniform("stir"), fp.uniform("env"), fp.uniform("wind")};

    if (!m_heat.build({W, H, GL_RG32F, true, std::string(EmbersShaders::BED) + EmbersShaders::FLICKER + (m_sketch ? EmbersShaders::POST_STEP : EmbersShaders::POST_FRAME)}, error))
        return fail(error ? *error : std::string{});
    const auto& hp = m_heat.program();
    m_postU = {hp.uniform("aN"), hp.uniform("rN"), hp.uniform("c2N"), hp.uniform("pop"), hp.uniform("popM"), hp.uniform("bed"), hp.uniform("flick")};
    if (!m_sketch) {
        // the per-frame post's decay over m = 0..63 steps (decayN's r and c2 (1 - r) / c1)
        GLfloat r[MAX_TRANSPORT_STEPS], c[MAX_TRANSPORT_STEPS];
        for (int m = 0; m < MAX_TRANSPORT_STEPS; m++) {
            const double rm = std::pow(1.0 - C1, m);
            r[m]            = static_cast<GLfloat>(rm);
            c[m]            = static_cast<GLfloat>(C2 * (1.0 - rm) / C1);
        }
        Gl::use(hp);
        glUniform1fv(hp.uniform("decayR"), MAX_TRANSPORT_STEPS, r);
        glUniform1fv(hp.uniform("decayC"), MAX_TRANSPORT_STEPS, c);
    }

    std::string       log;
    const std::string head    = m_sketch ? "#define BAKED 0\n" : "#define BAKED 1\n";
    auto              picture = Gl::compile(head + GLSL_BILINEAR + GLSL_BICUBIC + EmbersShaders::BED +
                                                        (m_sketch ? std::string(EmbersShaders::HASH) + EmbersShaders::COAL : std::string{}) + EmbersShaders::PICTURE, &log);
    if (!picture)
        return fail(std::format("the embers picture shader failed to compile: {}", log));
    m_picture = *picture;
    if (!m_health.build(HEALTH_CELL, {"heatTex", "fluidState"}, &log))
        return fail(log);
    if (!m_sketch) {
        auto bake = Gl::compile(std::string(EmbersShaders::HASH) + EmbersShaders::COAL + EmbersShaders::BAKE, &log);
        if (!bake)
            return fail(std::format("the embers bake shader failed to compile: {}", log));
        m_bakeProg = *bake;
        auto blurP = Gl::compile(EmbersShaders::BLUR, &log);
        if (!blurP)
            return fail(std::format("the embers blur shader failed to compile: {}", log));
        m_blurProg = *blurP;
        for (int i = 0; i < 2; i++) {
            m_blurTex[i] = Gl::texture(GL_R32F, W, H, GL_NEAREST);
            m_blurFb[i]  = Gl::framebuffer({m_blurTex[i]});
            if (!m_blurFb[i])
                return fail(std::format("the embers blur's {}x{} R32F framebuffer is incomplete", W, H));
        }
        m_blurU = {m_blurProg.uniform("src"), m_blurProg.uniform("dir"), m_blurProg.uniform("wk")};
        // the Gaussian's weights for |i| = 0..14, normalised over the 29 taps
        const double sig = ringSigma(14.0);
        double       wk[15], sum = 0;
        for (int i = 0; i <= 14; i++) {
            wk[i] = std::exp(-static_cast<double>(i * i) / (2 * sig * sig));
            sum += i == 0 ? wk[i] : 2 * wk[i];
        }
        GLfloat wf[15];
        for (int i = 0; i <= 14; i++)
            wf[i] = static_cast<GLfloat>(wk[i] / sum);
        Gl::use(m_blurProg);
        glUniform1fv(m_blurU.wk, 15, wf);
    }
    const auto& pp = m_picture;
    m_picU         = {pp.uniform("heat"), pp.uniform("prox"), pp.uniform("keyA"), pp.uniform("keyB"), pp.uniform("blend"),
                      pp.uniform("view"), pp.uniform("picBed"), pp.uniform("pulseTheta"), pp.uniform("fade")};
    // the programs of the other path are not compiled: their locations stay -1
    if (m_sketch)
        m_picCoalU = {-1, pp.uniform("creepI"), pp.uniform("creepF"), pp.uniform("theta")};
    if (m_bakeProg.id)
        m_bakeCoalU = {m_bakeProg.uniform("invShort"), m_bakeProg.uniform("creepI"), m_bakeProg.uniform("creepF"), m_bakeProg.uniform("theta")};

    // per lattice: the noise's periods and drifts (the sketch's frame, swapped in portrait)
    const SPeriods per      = bedPeriods(W, H);
    const bool     portrait = W < H;
    GLfloat        drift[4] = {static_cast<GLfloat>(SHIFT * 3e-5), static_cast<GLfloat>(SHIFT * 1.1e-5), static_cast<GLfloat>(SHIFT * 2e-5), static_cast<GLfloat>(-SHIFT * 2.6e-5)};
    if (portrait) {
        std::swap(drift[0], drift[1]);
        std::swap(drift[2], drift[3]);
    }
    for (const Gl::SProgram* p : {&m_heat.program(), static_cast<const Gl::SProgram*>(&m_picture), static_cast<const Gl::SProgram*>(&m_bakeProg)})
        if (p->id) {
            Gl::use(*p);
            glUniform2i(p->uniform("hashBase"), m_hashBase[0], m_hashBase[1]);
            glUniform4i(p->uniform("bedPer"), per.bed1[0], per.bed1[1], per.bed2[0], per.bed2[1]);
            glUniform2i(p->uniform("flickPer"), per.flick[0], per.flick[1]);
            glUniform4fv(p->uniform("bedDrift"), 1, drift);
            glUniform2f(p->uniform("flickDir"), portrait ? 1.0f : 0.0f, portrait ? 0.0f : 1.0f);
        }

    m_w  = W;
    m_h  = H;
    m_pw = pw;
    m_ph = ph;
    std::string err;
    if (!m_sketch && !buildBake(false, &err))
        return fail(err);
    m_needInit = true;
    return true;
}

// The keys at the picture's size, a key per second (framesPerKey = fps), primed two
// strips a frame; keepShown as marble's (Marble.cpp): the shown keys stay on screen while
// the new ones prime from the coal time they show. The picture samples the keys at its
// own uv, so a held pair of the old size needs no map.
bool CEmbersLook::buildBake(bool keepShown, std::string* error) {
    const int frames = std::max(1, static_cast<int>(std::lround(m_fps)));
    m_bakeFrames     = frames;
    if (keepShown)
        m_clock.requestPrimeFromShown(m_bake.ready(), m_bake.blend());
    else
        m_clock.requestPrime();
    const bool made = keepShown ? m_bake.rebuild(m_pw, m_ph, BAKE_FORMAT, frames, error) : m_bake.build(m_pw, m_ph, BAKE_FORMAT, frames, error);
    if (!made)
        return false;
    m_error.clear();
    m_bake.setPrimeRows(2 * ((m_ph + frames - 1) / frames));
    Gl::use(m_bakeProg);
    glUniform1f(m_bakeCoalU.sizeInv, 1.0f / static_cast<float>(std::min(m_pw, m_ph)));
    return true;
}

float CEmbersLook::stepsPerSecond() const {
    return SPS;
}

void CEmbersLook::setFrameRate(float fps) {
    if (fps > 0)
        m_fps = fps;
}

void CEmbersLook::reset() {
    m_needInit = true;
    m_resets++;
    m_health.cancel(); // a verdict on the state before the reset is no longer true
    m_lastHealthy = true;
    m_errorDue    = false;
}

void CEmbersLook::initialise() {
    m_fluid.init();
    m_heat.load(std::vector<float>(static_cast<size_t>(m_w) * m_h * 4, 0.0f));
    m_steps = m_sinceInit = 0;
    m_events.restart();
    if (!m_sketch) {
        blur(); // the proximity is written before the picture reads it
        std::string err;
        if (!m_error.empty() || !m_bake.width()) {
            if (!buildBake(false, &err))
                m_error = err;
        } else {
            m_bake.restart();
            m_clock.requestPrime();
        }
    }
    m_tmax = m_umax = 0;
    m_checkedAt     = ~uint64_t{0};
    m_lastHealthy   = true;
    m_needInit      = false;
}

void CEmbersLook::fluidStep(const CEmbersEvents::SStep& e) {
    const double t        = static_cast<double>(m_steps);
    GLfloat      stir[12] = {};
    GLfloat      env[3]   = {};
    for (size_t i = 0; i < m_stirrers.size(); i++) {
        const SStir s   = stirrer(i, t, m_w, m_h);
        stir[i * 4 + 0] = static_cast<GLfloat>(s.x);
        stir[i * 4 + 1] = static_cast<GLfloat>(s.y);
        stir[i * 4 + 2] = static_cast<GLfloat>(s.vx);
        stir[i * 4 + 3] = static_cast<GLfloat>(s.vy);
        env[i]          = static_cast<GLfloat>(s.env);
    }
    Gl::use(m_fluid.program());
    glUniform4fv(m_forceU.stir, 3, stir);
    glUniform3fv(m_forceU.env, 1, env);
    glUniform2f(m_forceU.wind, static_cast<GLfloat>(e.wx), static_cast<GLfloat>(e.wy));
    m_fluid.step();
}

void CEmbersLook::setNoise(GLint bedU, GLint flickU, double step) const {
    const SNoiseTime o = noiseAt(step, m_w, m_h);
    glUniform4f(bedU, o.bed1[0], o.bed1[1], o.bed2[0], o.bed2[1]);
    if (flickU >= 0)
        glUniform1f(flickU, o.flick);
}

// The heat once over the frame's n steps (n <= 64), from tstart = m_steps - n: the last
// pop alive in the frame (pop.popR 0: none) set popK steps into it.
void CEmbersLook::heatPerFrame(int n, const CEmbersEvents::SStep& pop, int popK) {
    const SHeatFactors f = heatFactors(n);
    Gl::use(m_heat.program());
    glUniform1f(m_postU.aN, static_cast<GLfloat>(f.aN));
    glUniform1f(m_postU.rN, static_cast<GLfloat>(f.rN));
    glUniform1f(m_postU.c2N, static_cast<GLfloat>(f.c2N));
    setNoise(m_postU.bed, m_postU.flick, static_cast<double>(m_steps - static_cast<uint64_t>(n)));
    if (popK >= 0)
        glUniform4f(m_postU.pop, static_cast<GLfloat>(pop.popX), static_cast<GLfloat>(pop.popY), static_cast<GLfloat>(pop.popR), static_cast<GLfloat>(pop.popHeat));
    else
        glUniform4f(m_postU.pop, 0, 0, 0, 0);
    glUniform1f(m_postU.popM, static_cast<GLfloat>(popK >= 0 ? n - 1 - popK : 0));
    m_heat.step(m_fluid.velocity(), m_w, m_h, static_cast<float>(n));
}

// embers-proximity-blur: x then y, at lattice resolution
void CEmbersLook::blur() {
    Gl::use(m_blurProg);
    glUniform1i(m_blurU.src, 0);
    Gl::bind(0, m_heat.texture());
    glUniform2i(m_blurU.dir, 1, 0);
    Gl::pass(m_blurProg, m_blurFb[0], 0, 0, m_w, m_h);
    Gl::bind(0, m_blurTex[0]);
    glUniform2i(m_blurU.dir, 0, 1);
    Gl::pass(m_blurProg, m_blurFb[1], 0, 0, m_w, m_h);
}

void CEmbersLook::advance(int steps) {
    if (!m_w)
        return;
    // a new frame rate: a key a second again, the picture kept meanwhile; tried once per
    // frame-rate value (a failure stays in m_error and makes the look unhealthy)
    if (!m_sketch && m_error.empty() && std::lround(m_fps) != m_bakeFrames && m_bake.width()) {
        std::string err;
        if (!buildBake(true, &err))
            m_error = err;
    }
    if (m_needInit)
        initialise();
    if (steps > 0) {
        if (m_sketch) {
            // the sketch: the heat after every step, at that step's time and pop
            for (int i = 0; i < steps; i++) {
                const auto e = m_events.step(m_steps, m_w, m_h);
                fluidStep(e);
                Gl::use(m_heat.program());
                setNoise(m_postU.bed, m_postU.flick, static_cast<double>(m_steps));
                if (e.pop)
                    glUniform4f(m_postU.pop, static_cast<GLfloat>(e.popX), static_cast<GLfloat>(e.popY), static_cast<GLfloat>(e.popR), static_cast<GLfloat>(e.popHeat));
                else
                    glUniform4f(m_postU.pop, 0, 0, 0, 0);
                m_heat.step(m_fluid.velocity(), m_w, m_h, 1.0f);
                m_steps++;
            }
        } else {
            // embers-heat-per-frame: once per transport of at most 64 steps, on the last velocity
            for (int left = steps; left > 0;) {
                const int            n    = std::min(left, MAX_TRANSPORT_STEPS);
                CEmbersEvents::SStep pop  = {};
                int                  popK = -1;
                for (int k = 0; k < n; k++) {
                    const auto e = m_events.step(m_steps, m_w, m_h);
                    if (e.pop) {
                        pop  = e;
                        popK = k;
                    }
                    fluidStep(e);
                    m_steps++;
                }
                heatPerFrame(n, pop, popK);
                left -= n;
            }
            blur();
        }
        m_sinceInit += static_cast<uint64_t>(steps);
    }
    if (!m_sketch && m_bake.width() && (steps > 0 || !m_bake.ready()))
        bakeAdvance(steps);
}

// The coal layer's time inputs at a step (EmbersShaders::COAL), from double: per creeping
// fbm and octave the offset's integer part (taken modulo 2^32: the hash's arithmetic) and
// fraction, and the turning angle's theta.
void CEmbersLook::setCoal(const SCoalU& u, double step) const {
    const double tt     = step * CREEP;
    const double off[3] = {tt * 0.3, -tt * 0.3, tt * 0.5};
    GLint        oi[12];
    GLfloat      of[12];
    for (int j = 0; j < 3; j++) {
        double o = off[j];
        for (int k = 0; k < 4; k++, o *= 2.03) {
            const double fl = std::floor(o);
            oi[j * 4 + k]   = static_cast<GLint>(static_cast<uint32_t>(static_cast<int64_t>(fl)));
            of[j * 4 + k]   = static_cast<GLfloat>(o - fl);
        }
    }
    glUniform4iv(u.creepI, 3, oi);
    glUniform4fv(u.creepF, 3, of);
    glUniform1f(u.theta, static_cast<GLfloat>(std::fmod(tt / 256.0, TAU)));
}

// One frame of the bake: priming or this frame's strip of the back key; the key times
// from the clock (CKeyClock), as marble's.
void CEmbersLook::bakeAdvance(int steps) {
    const double nominal = SPS * m_bake.framesPerKey() / std::max(m_fps, 1.0f);
    m_clock.frame(static_cast<double>(m_steps), nominal, m_bake.ready(), m_bake.starting(), steps);
    Gl::use(m_bakeProg);
    m_bake.advance([&](GLuint fbo, int y0, int y1) {
        setCoal(m_bakeCoalU, m_clock.keyTime(m_bake.bakingKey()));
        Gl::pass(m_bakeProg, fbo, 0, y0, m_bake.width(), y1 - y0);
    });
    if (m_bake.rotated())
        m_clock.rotated();
}

void CEmbersLook::draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) {
    if (!m_w || !m_picture.id)
        return;
    const bool shown = m_sketch || m_bake.showing(); // black until a whole pair exists
    Gl::use(m_picture);
    Gl::bind(0, m_heat.texture());
    glUniform1i(m_picU.heat, 0);
    if (m_sketch)
        setCoal(m_picCoalU, static_cast<double>(m_steps));
    else {
        Gl::bind(1, m_blurTex[1]);
        Gl::bind(2, m_bake.previous());
        Gl::bind(3, m_bake.next());
        glUniform1i(m_picU.prox, 1);
        glUniform1i(m_picU.keyA, 2);
        glUniform1i(m_picU.keyB, 3);
        glUniform1f(m_picU.blend, m_bake.blend());
    }
    glUniform2f(m_picU.view, static_cast<float>(m_pw), static_cast<float>(m_ph));
    setNoise(m_picU.bed, -1, static_cast<double>(m_steps));
    glUniform1f(m_picU.pulse, static_cast<GLfloat>(std::fmod(static_cast<double>(m_steps) * PULSE / 510.0, TAU)));
    glUniform1f(m_picU.fade, shown ? std::clamp(fade, 0.0f, 1.0f) : 0.0f);
    Gl::pass(m_picture, fbo, x, y, w, h, proj);
}

// One reduction of the heat and the density per check, none when nothing stepped since. A bake that could
// not be remade is unhealthy: the plugin resets the look, which remakes it.
void CEmbersLook::checkHealth() {
    if (!m_error.empty()) {
        m_errorDue = true;
        return;
    }
    if (!m_w || m_needInit || m_sinceInit == 0 || m_sinceInit == m_checkedAt || m_health.pending())
        return;
    if (m_health.start({m_heat.texture(), m_fluid.state()}, m_w, m_h))
        m_checkedAt = m_sinceInit;
}

EHealth CEmbersLook::takeHealth(bool wait) {
    if (std::exchange(m_errorDue, false))
        return EHealth::BLOWN;
    SReduced r;
    if (!m_health.take(r, wait))
        return wait ? (m_lastHealthy && m_error.empty() ? EHealth::HEALTHY : EHealth::BLOWN) : EHealth::NONE;
    m_umax   = r.max[0];
    m_tmax   = r.max[1];
    m_rhoMin = -r.max[2];
    // alive: judged at the step count the check was started at
    const bool alive = m_checkedAt < ALIVE_AFTER || m_tmax > TMIN_ALIVE;
    m_lastHealthy    = r.finite() && m_umax <= UMAX_BLOWN && m_tmax <= TMAX_RUNAWAY && m_rhoMin > 0.0f && alive;
    return m_lastHealthy ? EHealth::HEALTHY : EHealth::BLOWN;
}

std::string CEmbersLook::status() const {
    std::string path;
    if (m_sketch)
        path = "path sketch (heat per step, the 32-tap ring, coals per pixel; forced, bench only)";
    else {
        const std::string keys = m_bake.ready()     ? std::format("shown coal time {:.0f} steps", m_clock.shown(true, m_bake.blend()))
                                 : m_bake.holding() ? std::format("priming new keys, the old pair held at shown coal time {:.0f} steps", m_clock.shown(false, 0.0f))
                                                    : std::string{"priming (black)"};
        path = std::format("path accepted (embers-heat-per-frame, embers-proximity-blur, embers-bake): bake {}x{} RGBA8, key every {} frames, {}", m_bake.width(), m_bake.height(),
                           m_bake.framesPerKey(), keys);
    }
    return std::format("embers: lattice {}x{}, picture {}x{}, {} steps, {}, heat max {:.3f}, |u| max {:.4f}, rho min {:.4f}, {} pops (last at step {}), {} gusts (last at step {}), {} resets{}", m_w,
                       m_h, m_pw, m_ph, m_steps, path, m_tmax, m_umax, m_rhoMin, m_events.pops(), m_events.lastPopAt(), m_events.gusts(), m_events.lastGustAt(), m_resets,
                       m_error.empty() ? std::string{} : ", bake error: " + m_error);
}
