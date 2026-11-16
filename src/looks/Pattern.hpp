#pragma once

// The look `pattern` (look D2e, reference/2026-11-15-look-D2e-locked.md): a calm
// Gray-Scott web carried by a small D2Q9 fluid that three uneven stirrers keep moving,
// with a rare sudden event (burst, wipe, gust). Ported from the left card of
// reference/d2e-sketch.html; the template the next looks follow:
//
// - The lattice is SGeometry's (short side 225 cells) whatever the quality: the quality
//   only changes the picture's target, which the caller owns. build() keeps the state
//   when only the quality changes.
// - Orientation: the simulation runs in the sketch's coordinates (lattice row 0 = the
//   bottom of the picture), so the sketch's numbers are used unchanged; the picture
//   pass flips once (PatternShaders.hpp).
// - The look is defined per lattice cell: stirrer orbit amplitudes and speeds, the force
//   radius, event radii, the gust's speed and the web stay in cells on every lattice.
//   Only places are proportional: the stirrers' orbit centres (the sketch's 400x225
//   layout spread along the long side; on a portrait lattice the sketch's long axis runs
//   along y), event and periodic-seed positions (fractions of W and H), and the number of
//   initial spots (by area).
// - Per monitor: the seed (makeLook) sets the stirrers' phases, the feed/kill noise's
//   offsets, the event sequence and the first event's time, the periodic seeds' positions
//   and the initial spots; the same seed gives the same run.
// - Health: a GPU reduction (CReduction) of the chemistry (its .w is the fluid's |u|) and
//   the fluid's density, read through a fence without waiting: a non-finite value, |u|
//   over 0.4, a density at or below 0, or a web that vanished or filled the lattice; true
//   until the first step after (re)initialising, and the last answer when nothing stepped
//   since.
//   reset() only marks the state: the GL work happens in the next advance().
//
// Per fluid step: 1 fluid pass + 6 chemistry passes; per drawn frame: 1 picture pass.

#include "core/Fluid.hpp"
#include "core/Look.hpp"
#include "core/Reduce.hpp"
#include "gl/Gl.hpp"

#include <array>
#include <cstdint>
#include <string>

class CPatternLook final : public ILook {
  public:
    static constexpr int   CHEM_STEPS = 6;     // chemistry steps per fluid step
    static constexpr float FLUID_SPS  = 60.0f; // fluid steps per second (the sketch's frames)

    explicit CPatternLook(uint64_t seed);
    ~CPatternLook() override;
    CPatternLook(const CPatternLook&)            = delete;
    CPatternLook& operator=(const CPatternLook&) = delete;

    bool        build(const SGeometry& g, float quality, std::string* error) override;
    float       stepsPerSecond() const override; // fluid steps
    void        advance(int steps) override;     // fluid steps, CHEM_STEPS chemistry steps each
    void        draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) override;
    void        checkHealth() override;
    EHealth     takeHealth(bool wait) override;
    void        reset() override;
    std::string status() const override;
    float       meanDensity() override { return m_fluid.stats().rhoMean; }

    struct SStir {
        double x, y, vx, vy, env; // cells, cells per fluid step, strength
    };
    // stirrer i (0..2) at fluid step t on a w x h lattice; no GL
    SStir stirrer(size_t i, double t, int w, int h) const;

  private:
    struct SStirrer {
        double cx, cy, ax, ay, w1, w2, p1, p2, e1, e2; // the sketch's ST entry, phases offset per monitor
    };
    struct SGust {
        uint64_t t0;
        double   x, y, vx, vy, dur;
    };
    struct SSpot { // a disc of the step's source (seed uniform): periodic seed, burst, wipe
        double x, y, r, w;
        int    n; // chemistry steps left
    };

    void release();
    void initialise();
    void fluidStep();
    void chemStep();
    void scheduleEvent();

    uint64_t m_seed;
    int      m_w = 0, m_h = 0;

    CFluid       m_fluid;
    GLuint       m_chem[2] = {}, m_fb[2] = {};
    int          m_cur = 0;
    Gl::SProgram m_step, m_picture;
    struct {
        GLint chem = -1, vel = -1, size = -1, invShort = -1, n1i = -1, n1f = -1, n2i = -1, n2f = -1, seed = -1;
    } m_stepU;
    struct {
        GLint chem = -1, fade = -1;
    } m_picU;
    struct {
        GLint stir = -1, env = -1, drag = -1;
    } m_forceU;

    // per monitor, from the seed
    std::array<SStirrer, 3> m_stirrers{};
    double                  m_noise[4] = {}; // the feed/kill noise's offsets (two octaves, x and y)
    uint64_t                m_eventBase = 0, m_seedBase = 0;

    // the run
    bool     m_needInit = true;
    uint64_t m_fsteps = 0, m_csteps = 0, m_sinceInit = 0;
    uint64_t m_events = 0, m_nextEvent = 0, m_spots = 0;
    bool     m_hasGust = false, m_hasPending = false;
    SGust    m_gust{};
    SSpot    m_pending{};
    int      m_lastKind = -1; // 0 burst, 1 wipe, 2 gust
    uint64_t m_lastEventAt = 0;
    uint64_t m_resets = 0;

    // the last health check
    CReduction m_health;
    float    m_cover = 0.0f, m_act = 0.0f, m_umax = 0.0f, m_rhoMin = 1.0f;
    uint64_t m_checkedAt   = ~uint64_t{0}; // m_sinceInit at the last check
    bool     m_lastHealthy = true;
};
