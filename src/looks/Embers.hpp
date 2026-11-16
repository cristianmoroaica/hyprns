#pragma once

// The look `embers` (look C4q, card "C4q · coral", reference/2026-11-15-look-C4q-locked.md):
// a dark crust of coals seen from above, coral light in thin wandering cracks near the hot
// patches; a heat field that a slow D2Q9 fluid (three uneven stirrers, a light wind,
// rare gusts) carries, kept alive by a drifting bed of sources with a flicker and rare
// pops. Ported from the approved speed-ups of the embers pair in reference/equivalence.html
// (FIRE_FORCE, FIRE_POST_N, FIRE_BLUR, COAL_BAKE, firePicSrc with BAKE, BLUR and HAND), whose
// left card is the C4q sketch (reference/c4q-sketch.html, 8 steps per frame at 60 fps:
// 480 steps per second). Pattern's and marble's conventions: sketch coordinates (y up),
// one flip in the picture pass.
//
// Paths (core/Verdicts.hpp; all three accepted, the plugin always takes them):
// - embers-heat-per-frame: the heat is transported once per frame by all its steps, with
//   compounded diffusion and decay and the sources' floors decayed to the frame's end
//   (FIRE_POST_N).
// - embers-proximity-blur: the coals' nearness to heat is two separable Gaussian passes of
//   the heat at lattice resolution (taps -14..14 cells, sigma from the sketch's ring).
// - embers-bake: the coal layer (edge distance, tint, break-up, crack width) is baked into
//   a CBake (RGBA8) at the picture's size, a strip per frame, a key per second
//   (framesPerKey = fps); the picture is one tap of each shown key, blended.
// The sketch's own path (heat per step, the 32-tap ring, the coals per pixel per frame) is
// only built by hyprns-bench --force-sketch-path, to measure against.
//
// Time: the steps run at 480 per second times time_scale. A frame runs at most 64 steps
// per heat transport (the post's source loop is capped at 64; the plugin's clock never
// gives more, a longer frame in the bench is cut into transports of 64). The coals' time
// is the step count, latched per key by CKeyClock as marble's stone.
//
// The look is defined per lattice cell (short side 225): the stirrers' orbits, speeds and
// radius, the wind and gusts, the pops' radius, the diffusion, the blur's reach, the
// bed's and the flicker's noise (about 40 x 37.5, 25 x 25 and 12.5 cells per unit,
// rounded along the long side to a whole number of periods, so the field stays periodic)
// and their drift; the coals in units of the picture's short side (24 per short side).
// Proportional: the stirrers' orbit centres and the pops' places (fractions of the lattice,
// the sketch's long axis along the long side).
//
// Health: a GPU reduction (CReduction) of the heat (lattice-sized RG32F, .g = |u| of the
// last step) and the fluid's density, read through a fence without waiting: a non-finite
// value, |u| over 0.4, a density at or below 0, heat over 1.75 (no source sets more than the pop's 1.7
// and nothing in the transport makes new extrema) or a dead bed (no cell over 0.05 once
// 960 steps have run: the bed's sources keep some cells at 0.35 or more) is unhealthy;
// none when nothing stepped since.

#include "core/Bake.hpp"
#include "core/Fields.hpp"
#include "core/Fluid.hpp"
#include "core/Look.hpp"
#include "core/Reduce.hpp"
#include "gl/Gl.hpp"

#include <array>
#include <cstdint>
#include <string>

// The pops and gusts of the page's frame loop, step for step; no GL (unit tested). One
// event counter for both, starting at `base` (the page's starts at 0): pop k at
// fractions (k PLASTIC, k 0.5698402910) of the lattice, radius 7 + 6 frac(k 0.3819660113)
// cells, heat 1.7 for 30 steps, the next 3000..9000 steps later; gust k along angle
// k 2.39996 for 1800 steps, the next 12000..25000 steps later. The first pop at step 2500,
// the first gust at 7000. Positions and directions in the sketch's frame (x along the
// long side), swapped on a portrait lattice.
class CEmbersEvents {
  public:
    struct SStep {
        double wx = 0, wy = 0;                                // the wind with the gust, lattice units
        bool   pop = false;                                   // a pop alive at this step
        double popX = 0, popY = 0, popR = 0, popHeat = 0;    // cells
    };
    explicit CEmbersEvents(uint64_t base) : m_base(base) { restart(); }
    void     restart();
    // step s (0, 1, 2, ... in order after restart()) on a w x h lattice
    SStep    step(uint64_t s, int w, int h);
    uint64_t pops() const { return m_pops; }
    uint64_t gusts() const { return m_gusts; }
    uint64_t lastPopAt() const { return m_lastPopAt; }
    uint64_t lastGustAt() const { return m_gustT0; }
    double   nextPop() const { return m_nextPop; }
    double   nextGust() const { return m_nextGust; }

  private:
    uint64_t m_base;
    uint64_t m_ev = 0, m_pops = 0, m_gusts = 0, m_lastPopAt = 0, m_gustT0 = 0;
    double   m_nextPop = 0, m_nextGust = 0;
    bool     m_popAlive = false, m_gustAlive = false;
    int      m_popLeft  = 0;
    double   m_px = 0, m_py = 0, m_pr = 0, m_gcx = 0, m_gcy = 0;
};

class CEmbersLook final : public ILook {
  public:
    static constexpr float  SPS   = 480.0f; // steps per second (8 per frame at the sketch's 60)
    static constexpr double KAPPA = 0.02, C1 = 4e-4, C2 = 3e-4, LO = 0.66, HI = 0.76, SHIFT = 10.0, DRAG = 1.2e-3;
    static constexpr int    MAX_TRANSPORT_STEPS = 64; // the heat post's source loop
    // the post's hoisted noise corners (corners()/fnpC) cover a frame's drift only while
    // it stays under one noise cell: the flicker moves 4e-3 a step, the bed SHIFT * 3e-5;
    // and the decay table in the shader has 64 entries
    static_assert(MAX_TRANSPORT_STEPS * 4e-3 < 1.0 && MAX_TRANSPORT_STEPS * SHIFT * 3e-5 < 1.0 && MAX_TRANSPORT_STEPS == 64);

    // sketchPath: the sketch's per-step, per-pixel path (hyprns-bench --force-sketch-path)
    explicit CEmbersLook(uint64_t seed, bool sketchPath = false);
    ~CEmbersLook() override;
    CEmbersLook(const CEmbersLook&)            = delete;
    CEmbersLook& operator=(const CEmbersLook&) = delete;

    bool        build(const SGeometry& g, float quality, std::string* error) override;
    float       stepsPerSecond() const override;
    void        advance(int steps) override;
    void        draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) override;
    void        checkHealth() override;
    EHealth     takeHealth(bool wait) override;
    void        reset() override;
    std::string status() const override;
    bool        settled() const override { return m_sketch || m_bake.ready(); } // the bake pair is whole
    float       meanDensity() override { return m_fluid.stats().rhoMean; }
    void        setFrameRate(float fps) override;

    // The heat moved once by n steps (FIRE_POST_N): diffusion aN = 1 - (1 - 4 kappa)^n,
    // decay rN = (1 - c1)^n and c2N = c2 (1 - rN) / c1. No GL.
    struct SHeatFactors {
        double aN, rN, c2N;
    };
    static SHeatFactors heatFactors(int n);
    // a floor f decayed over m steps: max(f (1 - c1)^m - c2 (1 - (1 - c1)^m) / c1, 0)
    static double decayN(double f, int m);

    // The bed's two noise layers' and the flicker's periods (noise cells per lattice axis)
    // on a w x h lattice: the sketch's (10, 6), (16, 9) and (32, 18) on 400x225, the long
    // axis' rounded in proportion to it. No GL.
    struct SPeriods {
        int bed1[2], bed2[2], flick[2];
    };
    static SPeriods bedPeriods(int w, int h);
    // The sketch's drift of the bed's layers and the flicker at step t, reduced modulo
    // their periods (in double, so float holds them however long the look runs): bed1 is
    // added, bed2 subtracted, flick added along the short axis. No GL.
    struct SNoiseTime {
        float bed1[2], bed2[2], flick;
    };
    static SNoiseTime noiseAt(double step, int w, int h);

    struct SStir {
        double x, y, vx, vy, env; // cells, cells per step, strength
    };
    // stirrer i (0..2) at step t on a w x h lattice; no GL
    SStir    stirrer(size_t i, double t, int w, int h) const;
    uint64_t eventBase() const { return m_eventBase; }

  private:
    struct SStirrer {
        double cx, cy, ax, ay, w1, w2, p1, p2, e1, e2;
    };
    struct SCoalU {
        GLint sizeInv = -1, creepI = -1, creepF = -1, theta = -1;
    };

    void release();
    bool buildBake(bool keepShown, std::string* error);
    void initialise();
    void fluidStep(const CEmbersEvents::SStep& e);
    void heatPerFrame(int n, const CEmbersEvents::SStep& pop, int popK);
    void blur();
    void bakeAdvance(int steps);
    void setCoal(const SCoalU& u, double step) const;
    void setNoise(GLint bedU, GLint flickU, double step) const;

    uint64_t m_seed;
    bool     m_sketch;

    int   m_w = 0, m_h = 0;   // lattice
    int   m_pw = 0, m_ph = 0; // the picture (monitor x quality), monitor orientation
    float m_fps = 30.0f;

    CFluid        m_fluid;
    CField        m_heat; // RG32F: heat, |u|
    CBake         m_bake;
    GLuint        m_blurTex[2] = {}, m_blurFb[2] = {};
    Gl::SProgram  m_blurProg, m_bakeProg, m_picture;
    CEmbersEvents m_events;

    struct {
        GLint stir = -1, env = -1, wind = -1;
    } m_forceU;
    struct {
        GLint aN = -1, rN = -1, c2N = -1, pop = -1, popM = -1, bed = -1, flick = -1;
    } m_postU;
    struct {
        GLint src = -1, dir = -1, wk = -1;
    } m_blurU;
    SCoalU m_bakeCoalU, m_picCoalU;
    struct {
        GLint heat = -1, prox = -1, keyA = -1, keyB = -1, blend = -1, view = -1, bed = -1, pulse = -1, fade = -1;
    } m_picU;

    std::array<SStirrer, 3> m_stirrers{};
    int                     m_hashBase[2] = {0, 0};
    uint64_t                m_eventBase   = 0;

    bool        m_needInit = true;
    uint64_t    m_steps = 0, m_sinceInit = 0, m_resets = 0;
    CKeyClock   m_clock;
    int         m_bakeFrames = 0;
    std::string m_error;

    CReduction m_health;
    float    m_tmax = 0, m_umax = 0, m_rhoMin = 1;
    uint64_t m_checkedAt   = ~uint64_t{0};
    bool     m_lastHealthy = true;
    bool     m_errorDue    = false; // a check found m_error: the next take says BLOWN
};
