#include "looks/Marble.hpp"
#include "core/Compound.hpp"
#include "core/Sequence.hpp"
#include "core/Verdicts.hpp"
#include "looks/MarbleShaders.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>
#include <vector>

static_assert(Verdicts::MARBLE_BAKE && Verdicts::MARBLE_DISPLACEMENT_PER_FRAME,
              "marble takes the accepted paths; a rejected one needs its own plugin path (only the bench builds the sketch's)");

namespace {

constexpr double SKETCH_W = 400.0, SKETCH_H = 225.0;
constexpr double TAU      = 6.283185307179586;

// The card's ST: centre, amplitude, angular speeds, phases, the unevenness' speeds (env),
// per fluid step.
constexpr std::array<std::array<double, 10>, 3> STIRRERS = {{
    {100, 112, 120, 70, 6.2e-4, 6.2e-4 * 1.6180339887, 0.0, 1.3, 1.8e-3, 1.8e-3 * 0.6180339887},
    {300, 112, 110, 80, 5.2e-4 * 1.4142135623, 5.2e-4, 2.1, 0.4, 1.5e-3, 1.5e-3 * 1.4142135623},
    {200, 60, 160, 50, 4.4e-4, 4.4e-4 * 2.2360679775, 4.0, 3.3, 2.1e-3, 2.1e-3 * 0.7548776662},
}};

constexpr float OMEGA = 1.85f;

// the stone's clocks (fluid steps): vein life 7200 x PER[k], brass epochs 14400
constexpr double LIFE = 7200.0, BRASS_EPOCH = 14400.0;
constexpr double PER[4]      = {1.0, 1.27, 1.618, 0.81};
constexpr double SEED_REDUCE = 509.0;

// Health: a fluid speed no stable run reaches (CFluid::stats()'s limit), and the most
// displacement a flow at that limit can hold (0.4 / (1 - 0.975))
constexpr float UMAX_BLOWN = 0.4f, DMAX_RUNAWAY = 16.0f;

// The health check's reduction, per cell: |u|, |d|^2, the density's min and max; the
// density summed; the displacement, |u| and the density must be finite.
constexpr const char* HEALTH_CELL = R"GLSL(
uniform sampler2D dispTex;    // dx, dy, |u|
uniform sampler2D fluidState; // f8, ux, uy, rho
void cell(ivec2 c, out vec4 mx, out vec3 sm, out vec4 chk) {
    vec4  d   = texelFetch(dispTex, c, 0);
    float rho = texelFetch(fluidState, c, 0).w;
    mx  = vec4(d.z, dot(d.xy, d.xy), -rho, rho);
    sm  = vec3(rho, 0.0, 0.0);
    chk = vec4(d.xyz, rho);
}
)GLSL";

// The keys: 8-bit linear colour, hardware-filtered (the dispatcher's choice); the picture
// pass dithers after the lighting
constexpr GLenum BAKE_FORMAT = GL_RGBA8;

const char* formatName(GLenum f) {
    return f == GL_SRGB8_ALPHA8 ? "SRGB8_ALPHA8" : "RGBA8";
}

} // namespace

CMarbleLook::CMarbleLook(uint64_t seed, bool sketchPath) : m_seed(seed), m_sketch(sketchPath) {
    uint64_t s = seed ^ 0x6d61726231ull; // "marb1": not pattern's sequence for the same monitor
    for (size_t i = 0; i < m_stirrers.size(); i++) {
        const auto& q = STIRRERS[i];
        m_stirrers[i] = {q[0], q[1], q[2], q[3], q[4], q[5], q[6] + TAU * uniform01(s), q[7] + TAU * uniform01(s), q[8], q[9]};
    }
    m_hashBase[0] = static_cast<int>(uniform01(s) * 4096.0);
    m_hashBase[1] = static_cast<int>(uniform01(s) * 4096.0);
}

CMarbleLook::~CMarbleLook() {
    release();
}

void CMarbleLook::release() {
    m_bake.release();
    Gl::destroy(m_bakeProg);
    Gl::destroy(m_picture);
    m_health.release();
    m_w = m_h = m_pw = m_ph = 0;
}

CMarbleLook::SStoneTime CMarbleLook::stoneAt(double step) {
    SStoneTime st{};
    for (int k = 0; k < 4; k++) {
        const double ph = step / (LIFE * PER[k]) + 0.37 * k, epoch = std::floor(ph), f = ph - epoch;
        st.life[k]      = static_cast<float>(smoothstep(0.0, 0.3, f) * smoothstep(1.0, 0.7, f));
        st.seed[k]      = static_cast<float>(std::fmod(epoch * 17.31 + 41.7 * k, SEED_REDUCE));
    }
    const double bph = step / BRASS_EPOCH, bn = std::floor(bph);
    st.brass[0]      = static_cast<float>(std::fmod(bn * 9.1, SEED_REDUCE));
    st.brass[1]      = static_cast<float>(std::fmod((bn + 1.0) * 9.1, SEED_REDUCE));
    st.brass[2]      = static_cast<float>(smoothstep(0.0, 1.0, bph - bn));
    return st;
}

// As pattern's: amplitudes, speeds and the force radius in cells on every lattice, the
// orbit centre placed proportionally (the sketch's long axis along the lattice's long side).
CMarbleLook::SStir CMarbleLook::stirrer(size_t i, double t, int w, int h) const {
    const auto&  q        = m_stirrers[i];
    const bool   portrait = w < h;
    const double longS = (portrait ? h : w) / SKETCH_W, shortS = (portrait ? w : h) / SKETCH_H;
    const double X  = q.cx * longS + q.ax * std::sin(q.w1 * t + q.p1), Y = q.cy * shortS + q.ay * std::sin(q.w2 * t + q.p2);
    const double VX = q.ax * q.w1 * std::cos(q.w1 * t + q.p1), VY = q.ay * q.w2 * std::cos(q.w2 * t + q.p2);
    const double env = 1.6 * smoothstep(-0.2, 0.7, std::sin(q.e1 * t + q.p1) + 0.6 * std::sin(q.e2 * t + q.p2));
    return portrait ? SStir{Y, X, VY, VX, env} : SStir{X, Y, VX, VY, env};
}


bool CMarbleLook::build(const SGeometry& g, float quality, std::string* error) {
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
    if (g.cellsW < 8 || g.cellsH < 8 || pw < 1 || ph < 1)
        return fail(std::format("a lattice of {}x{} cells, a picture of {}x{}", g.cellsW, g.cellsH, pw, ph));

    if (!m_fluid.build({g.cellsW, g.cellsH, false, OMEGA, MarbleShaders::FORCE}, error))
        return fail(error ? *error : std::string{});
    const auto& fp = m_fluid.program();
    m_forceU       = {fp.uniform("stir"), fp.uniform("env")};
    if (!m_disp.build({g.cellsW, g.cellsH, GL_RGBA32F, true, MarbleShaders::POST}, error))
        return fail(error ? *error : std::string{});
    m_postU = {m_disp.program().uniform("relaxN"), m_disp.program().uniform("gsum")};

    std::string       log;
    const std::string head    = m_sketch ? "#define BAKED 0\n" : "#define BAKED 1\n";
    auto              picture = Gl::compile(head + GLSL_BILINEAR + (m_sketch ? MarbleShaders::STONE : "") + MarbleShaders::PICTURE, &log);
    if (!picture)
        return fail(std::format("the marble picture shader failed to compile: {}", log));
    m_picture = *picture;
    if (!m_health.build(HEALTH_CELL, {"dispTex", "fluidState"}, &log))
        return fail(log);
    if (!m_sketch) {
        auto bake = Gl::compile(std::string(MarbleShaders::STONE) + MarbleShaders::BAKE, &log);
        if (!bake)
            return fail(std::format("the marble bake shader failed to compile: {}", log));
        m_bakeProg = *bake;
    }
    const auto& pp = m_picture;
    m_picU         = {pp.uniform("disp"), pp.uniform("keyA"), pp.uniform("keyB"), pp.uniform("blend"), pp.uniform("bakeMap"),
                      pp.uniform("view"), pp.uniform("dScale"), pp.uniform("light"), pp.uniform("fade")};
    m_picStoneU    = {pp.uniform("veinLife"), pp.uniform("veinSeed"), pp.uniform("brassSeed")};
    m_bakeStoneU   = {m_bakeProg.uniform("veinLife"), m_bakeProg.uniform("veinSeed"), m_bakeProg.uniform("brassSeed")};
    m_bakeU        = {m_bakeProg.uniform("invShort"), m_bakeProg.uniform("margin")};
    for (const auto* p : {&m_picture, &m_bakeProg})
        if (p->id) {
            Gl::use(*p);
            glUniform2i(p->uniform("hashBase"), m_hashBase[0], m_hashBase[1]);
        }

    m_w  = g.cellsW;
    m_h  = g.cellsH;
    m_pw = pw;
    m_ph = ph;
    std::string err;
    if (!m_sketch && !buildBake(false, &err))
        return fail(err);
    m_needInit = true;
    return true;
}

// The keys at the picture's size plus the margin, a key per second (framesPerKey = fps),
// primed two strips a frame (a key period, the cost of one more strip per frame than the
// steady state; whole in one frame would be 189 ms on the iGPU at 3440x1440). keepShown
// (a new size or frame rate, the fluid kept): the shown keys stay on screen meanwhile and
// the new ones prime from the stone time they show, so the picture neither goes black
// nor jumps; otherwise (a new lattice) the picture is black until they are whole.
bool CMarbleLook::buildBake(bool keepShown, std::string* error) {
    const int shortSide = std::min(m_pw, m_ph);
    m_margin            = static_cast<int>(std::ceil(MARGIN_CELLS * shortSide / SKETCH_H));
    const int frames    = std::max(1, static_cast<int>(std::lround(m_fps)));
    const int bw = m_pw + 2 * m_margin, bh = m_ph + 2 * m_margin;
    m_bakeFrames        = frames;
    if (keepShown)
        m_clock.requestPrimeFromShown(m_bake.ready(), m_bake.blend());
    else
        m_clock.requestPrime();
    const bool made = keepShown ? m_bake.rebuild(bw, bh, BAKE_FORMAT, frames, error) : m_bake.build(bw, bh, BAKE_FORMAT, frames, error);
    if (!made)
        return false;
    m_error.clear();
    m_bake.setPrimeRows(2 * ((bh + frames - 1) / frames));
    const float S = static_cast<float>(shortSide);
    m_bake.setMap({S / static_cast<float>(bw), S / static_cast<float>(bh), static_cast<float>(m_margin) / static_cast<float>(bw),
                   static_cast<float>(m_margin) / static_cast<float>(bh)});
    Gl::use(m_bakeProg);
    glUniform1f(m_bakeU.invShort, 1.0f / S);
    glUniform1f(m_bakeU.margin, static_cast<float>(m_margin));
    return true;
}

float CMarbleLook::stepsPerSecond() const {
    return FLUID_SPS;
}

void CMarbleLook::setFrameRate(float fps) {
    if (fps > 0)
        m_fps = fps;
}

void CMarbleLook::reset() {
    m_needInit = true;
    m_resets++;
    m_health.cancel(); // a verdict on the state before the reset is no longer true
    m_lastHealthy = true;
    m_errorDue    = false;
}

void CMarbleLook::initialise() {
    m_fluid.init();
    m_disp.load(std::vector<float>(static_cast<size_t>(m_w) * m_h * 4, 0.0f));
    m_steps = m_sinceInit = 0;
    if (!m_sketch) {
        // a new look or a reset starts black: the keys prime over a key period. A bake
        // that could not be remade is made again here, from nothing.
        std::string err;
        if (!m_error.empty() || !m_bake.width()) {
            if (!buildBake(false, &err))
                m_error = err;
        } else {
            m_bake.restart();
            m_clock.requestPrime();
        }
    }
    m_checkedAt   = ~uint64_t{0};
    m_lastHealthy = true;
    m_needInit    = false;
}

void CMarbleLook::fluidStep() {
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
    m_fluid.step();
    m_steps++;
}

void CMarbleLook::setStone(const SStoneU& u, double step) const {
    const SStoneTime st = stoneAt(step);
    glUniform4fv(u.life, 1, st.life);
    glUniform4fv(u.seed, 1, st.seed);
    glUniform3fv(u.brass, 1, st.brass);
}

void CMarbleLook::advance(int steps) {
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
            // the sketch: the displacement moves after every fluid step
            Gl::use(m_disp.program());
            glUniform1f(m_postU.relaxN, static_cast<GLfloat>(RELAX));
            glUniform1f(m_postU.gsum, static_cast<GLfloat>(RELAX));
            for (int i = 0; i < steps; i++) {
                fluidStep();
                m_disp.step(m_fluid.velocity(), m_w, m_h, 1.0f);
            }
        } else {
            // marble-displacement-per-frame: once, by all the frame's steps, on the last velocity
            for (int i = 0; i < steps; i++)
                fluidStep();
            const SRelaxSum rs = relaxSum(RELAX, steps);
            Gl::use(m_disp.program());
            glUniform1f(m_postU.relaxN, static_cast<GLfloat>(rs.power));
            glUniform1f(m_postU.gsum, static_cast<GLfloat>(rs.sum));
            m_disp.step(m_fluid.velocity(), m_w, m_h, static_cast<float>(steps));
        }
        m_sinceInit += static_cast<uint64_t>(steps);
    }
    if (!m_sketch && m_bake.width() && (steps > 0 || !m_bake.ready()))
        bakeAdvance(steps);
}

// One frame of the bake: priming (after a build, rebuild or reset) or this frame's strip
// of the back key; the key times come from the clock (CKeyClock).
void CMarbleLook::bakeAdvance(int steps) {
    const double nominal = FLUID_SPS * m_bake.framesPerKey() / std::max(m_fps, 1.0f);
    m_clock.frame(static_cast<double>(m_steps), nominal, m_bake.ready(), m_bake.starting(), steps);
    Gl::use(m_bakeProg);
    m_bake.advance([&](GLuint fbo, int y0, int y1) {
        setStone(m_bakeStoneU, m_clock.keyTime(m_bake.bakingKey()));
        Gl::pass(m_bakeProg, fbo, 0, y0, m_bake.width(), y1 - y0);
    });
    if (m_bake.rotated())
        m_clock.rotated();
}

void CMarbleLook::draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) {
    if (!m_w || !m_picture.id)
        return;
    const bool  shown = m_sketch || m_bake.showing(); // black until a whole pair exists
    const float S     = static_cast<float>(std::min(m_pw, m_ph));
    Gl::use(m_picture);
    Gl::bind(0, m_disp.texture());
    glUniform1i(m_picU.disp, 0);
    if (m_sketch)
        setStone(m_picStoneU, static_cast<double>(m_steps));
    else {
        Gl::bind(1, m_bake.previous());
        Gl::bind(2, m_bake.next());
        glUniform1i(m_picU.keyA, 1);
        glUniform1i(m_picU.keyB, 2);
        glUniform1f(m_picU.blend, m_bake.blend());
        const auto& map = m_bake.shownMap(); // the held pair's own, while one is shown
        glUniform4f(m_picU.bakeMap, map[0], map[1], map[2], map[3]);
    }
    glUniform2f(m_picU.view, static_cast<float>(m_pw), static_cast<float>(m_ph));
    glUniform2f(m_picU.dScale, static_cast<float>(m_pw) / S / static_cast<float>(m_w), static_cast<float>(m_ph) / S / static_cast<float>(m_h));
    glUniform3f(m_picU.light, 0.35f * static_cast<float>(m_pw) / S, 0.80f * static_cast<float>(m_ph) / S, 0.55f);
    glUniform1f(m_picU.fade, shown ? std::clamp(fade, 0.0f, 1.0f) : 0.0f);
    Gl::pass(m_picture, fbo, x, y, w, h, proj);
}

// One reduction of the displacement and the density per check, none when nothing stepped
// since. A bake that could not be remade is unhealthy: the plugin resets the look, which
// remakes it.
void CMarbleLook::checkHealth() {
    if (!m_error.empty()) {
        m_errorDue = true;
        return;
    }
    if (!m_w || m_needInit || m_sinceInit == 0 || m_sinceInit == m_checkedAt || m_health.pending())
        return;
    if (m_health.start({m_disp.texture(), m_fluid.state()}, m_w, m_h))
        m_checkedAt = m_sinceInit;
}

EHealth CMarbleLook::takeHealth(bool wait) {
    if (std::exchange(m_errorDue, false))
        return EHealth::BLOWN;
    SReduced r;
    if (!m_health.take(r, wait))
        return wait ? (m_lastHealthy && m_error.empty() ? EHealth::HEALTHY : EHealth::BLOWN) : EHealth::NONE;
    m_umax        = r.max[0];
    m_dmax        = std::sqrt(std::max(0.0f, r.max[1]));
    m_rhoMin      = -r.max[2];
    m_lastHealthy = r.finite() && m_umax <= UMAX_BLOWN && m_dmax <= DMAX_RUNAWAY && m_rhoMin > 0.0f;
    return m_lastHealthy ? EHealth::HEALTHY : EHealth::BLOWN;
}

std::string CMarbleLook::status() const {
    std::string path;
    if (m_sketch)
        path = "path sketch (stone per pixel, displacement per step; forced, bench only)";
    else {
        const std::string keys = m_bake.ready()     ? std::format("shown stone time {:.0f} steps", m_clock.shown(true, m_bake.blend()))
                                 : m_bake.holding() ? std::format("priming new keys, the old pair held at shown stone time {:.0f} steps", m_clock.shown(false, 0.0f))
                                                    : std::string{"priming (black)"};
        path = std::format("path bake (marble-bake, marble-displacement-per-frame accepted): bake {}x{} {}, key every {} frames, {}", m_bake.width(), m_bake.height(),
                           formatName(m_bake.internal()), m_bake.framesPerKey(), keys);
    }
    return std::format("marble: lattice {}x{}, picture {}x{}, {} fluid steps, {}, |d| max {:.2f} cells, |u| max {:.4f}, rho min {:.4f}, {} resets{}", m_w, m_h, m_pw, m_ph, m_steps,
                       path, m_dmax, m_umax, m_rhoMin, m_resets, m_error.empty() ? std::string{} : ", bake error: " + m_error);
}
