#pragma once

// The look `marble` (look A2n, reference/2026-11-15-look-A2n-chosen.md): black stone
// with thin pale veins that live and die over minutes, faint brass, a sheen at the upper
// left; a small D2Q9 fluid that three uneven stirrers keep moving carries a displacement
// field through which the stone is seen, so it slowly drifts and swirls and relaxes
// back. Ported from the second card of reference/a2n-sketch.html, pattern's conventions
// (Pattern.hpp): sketch coordinates (y up), one flip in the picture pass.
//
// Paths (core/Verdicts.hpp; both accepted, the plugin always takes them):
// - marble-bake: the stone (colour and height) is baked into a CBake at the picture's
//   size plus a margin, one strip per frame, a key per second (framesPerKey = fps); the
//   picture is one tap of the two shown keys, blended.
// - marble-displacement-per-frame: the displacement moves once per frame by all its
//   fluid steps, d r^n + u r (1 - r^n)/(1 - r).
// The sketch's own path (the stone per pixel per frame, the displacement per step) is
// only built by hyprns-bench --force-sketch-path, to measure against.
//
// Time: the stone's time is the fluid step count. A key's time is latched when the
// bake starts it: the next key's time plus the steps the last key period ran, so keys
// stay evenly spaced at any time_scale and the shown stone runs at the clock's pace.
// No step, no bake: time_scale 0 freezes the stone.
//
// The look is defined per lattice cell (short side 225): stirrer orbits, speeds and
// radius, and the displacement in cells; the stone's features in units of the short
// side. Proportional: the stirrers' orbit centres (as pattern's) and the light's place
// (0.35 of the width, 0.8 of the height, as in the sketch).
//
// Health: a GPU reduction (CReduction) of the displacement (lattice-sized RGBA32F,
// .z = |u|) and the fluid's density, read through a fence without waiting: a non-finite
// value, |u| over 0.4, a density at or below 0, or |d| over 16 cells (the most a 0.4 flow
// can hold at relax 0.975 is 0.4 / 0.025 = 16) is a blow-up; none when nothing stepped
// since.

#include "core/Bake.hpp"
#include "core/Fields.hpp"
#include "core/Fluid.hpp"
#include "core/Look.hpp"
#include "core/Reduce.hpp"
#include "gl/Gl.hpp"

#include <array>
#include <cstdint>
#include <string>

class CMarbleLook final : public ILook {
  public:
    static constexpr float  FLUID_SPS    = 120.0f; // fluid steps per second (2 per frame at the sketch's 60)
    static constexpr double RELAX        = 0.975;  // the displacement's relax per step
    static constexpr int    MARGIN_CELLS = 8;      // the bake's margin around the picture, in cells

    // sketchPath: the sketch's per-pixel, per-step path (hyprns-bench --force-sketch-path)
    explicit CMarbleLook(uint64_t seed, bool sketchPath = false);
    ~CMarbleLook() override;
    CMarbleLook(const CMarbleLook&)            = delete;
    CMarbleLook& operator=(const CMarbleLook&) = delete;

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

    // The stone's time-dependent inputs at a fluid step (the sketch's `time`), from double:
    // per vein family its life (0..1) and seed sd; the brass epochs' two offsets and the
    // blend between them. Offsets are reduced modulo 509 (a different but equally random
    // place in the noise), so float holds them however long the look runs; below 509 they
    // are the sketch's own. No GL.
    struct SStoneTime {
        float life[4], seed[4], brass[3];
    };
    static SStoneTime stoneAt(double step);

    struct SStir {
        double x, y, vx, vy, env; // cells, cells per fluid step, strength
    };
    // stirrer i (0..2) at fluid step t on a w x h lattice; no GL
    SStir stirrer(size_t i, double t, int w, int h) const;

  private:
    struct SStirrer {
        double cx, cy, ax, ay, w1, w2, p1, p2, e1, e2;
    };

    struct SStoneU {
        GLint life = -1, seed = -1, brass = -1;
    };

    void release();
    // keepShown: the lattice stays; the shown keys stay on screen while the new ones prime
    bool buildBake(bool keepShown, std::string* error);
    void initialise();
    void fluidStep();
    void bakeAdvance(int steps);
    void setStone(const SStoneU& u, double step) const;

    uint64_t m_seed;
    bool     m_sketch;

    int   m_w = 0, m_h = 0;   // lattice
    int   m_pw = 0, m_ph = 0; // the picture (monitor x quality), monitor orientation
    int   m_margin = 0;       // the bake's margin, picture pixels
    float m_fps    = 30.0f;

    CFluid       m_fluid;
    CField       m_disp;
    CBake        m_bake;
    Gl::SProgram m_bakeProg, m_picture;
    struct {
        GLint stir = -1, env = -1;
    } m_forceU;
    struct {
        GLint relaxN = -1, gsum = -1;
    } m_postU;
    SStoneU m_bakeStoneU, m_picStoneU; // looked up once per build
    struct {
        GLint invShort = -1, margin = -1;
    } m_bakeU;
    struct {
        GLint disp = -1, keyA = -1, keyB = -1, blend = -1, bakeMap = -1, view = -1, dScale = -1, light = -1, fade = -1;
    } m_picU;

    std::array<SStirrer, 3> m_stirrers{};
    int                     m_hashBase[2] = {0, 0};

    bool     m_needInit = true;
    uint64_t m_steps = 0, m_sinceInit = 0, m_resets = 0;
    CKeyClock   m_clock;          // the keys' stone times (fluid steps)
    int         m_bakeFrames = 0; // the frames per key last asked of the bake
    std::string m_error;          // a bake that could not be remade: unhealthy until a reset remakes it

    CReduction m_health;
    float    m_dmax = 0, m_umax = 0, m_rhoMin = 1;
    uint64_t m_checkedAt   = ~uint64_t{0};
    bool     m_lastHealthy = true;
    bool     m_errorDue    = false; // a check found m_error: the next take says BLOWN
};
