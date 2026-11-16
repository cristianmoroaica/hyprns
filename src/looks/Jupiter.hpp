#pragma once

// The look `jupiter` (look B7, card "B7 · dark, through a camera",
// reference/2026-11-15-look-B7-chosen.md): bands of cloud running east and west past each
// other; every so often a cluster of storms breaks out along a band edge and tears them
// into swirls, and now and then a great eye with a dark core drifts along an odd band edge;
// a dark six-step palette seen through a camera (relief, colour fringing, contrast, a cool
// shadow tint, a vignette and sensor grain). Ported from the jupiter pair of
// reference/equivalence.html (JOVE_FORCE, JOVE_ADV_N(r16), JOVE_MAC_N(r16), JOVE_SHOW),
// whose left card is sketchJove of reference/b7-sketch.html (6 steps per frame at 60 fps:
// 360 steps per second).
//
// Paths (core/Verdicts.hpp; both accepted, the plugin always takes them):
// - jupiter-cloud-per-frame: the cloud is moved once per frame by all its steps (the
//   back-trace by velocity x n, every per-step mix factor a as 1 - (1 - a)^n, storm and
//   eye positions those of the frame's last step); the fluid takes every step.
// - jupiter-cloud-r16f: the cloud and its two MacCormack temporaries are R16F, read with
//   hardware linear filtering, written with unbiased stochastic rounding to a neighbouring
//   half float (sround16), so the slow relaxation toward the bands survives the store.
// The sketch's own path (the cloud after every step, R32F) is only built by
// hyprns-bench --force-sketch-path, to measure against.
//
// Coordinates: the sketch's (row 0 = the bottom, y up) in the lattice and the cloud, one
// flip in the picture pass. Walls at the first and last row (CFluid wallsY), periodic in x;
// the cloud wraps in x (GL_REPEAT and a conditional +-width on texel indices) and clamps
// in y. The cloud is round(4 x lattice x quality) each way (the sketch's K = 4 at q 1).
//
// Per lattice cell (short side 225): the jets' strength and relaxation, the beta term's
// gradient, the storms' radii (11..23), kick, spacing (55..85) and offsets from their edge
// (+-17), the eye's radius (20), shape and drift (0.004 cells per step), the band height
// (n jets: 7 when the lattice is 225 high, else the odd number nearest 7 H / 225, so a band
// stays about 32 cells high; the band profile's second term keeps its period per band).
// Proportional: the outbreaks' and the eyes' x (fractions of the width), the band edges
// (k H / n). The picture's camera is per picture (fringe and vignette in uv), its grain in
// cells of 1.5 monitor pixels.
//
// Time: 360 steps per second times time_scale; a frame's cloud transport covers at most 64
// steps (a longer frame in the bench is cut into transports of 64). The grain renews 24
// times a second of wall time (setWallClock), so it keeps moving at time_scale 0.
//
// Health: a GPU reduction (CReduction) of the fluid's state per check, read a few frames
// later through a fence without waiting (none when nothing stepped since): unhealthy on a
// non-finite value, |u| over 0.25 or a density at or below 0.

#include "core/Fluid.hpp"
#include "core/Look.hpp"
#include "core/Reduce.hpp"
#include "gl/Gl.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// A storm alive at the current step: lattice cells, sketch coordinates (y up).
struct SStorm {
    double   x, y, r;
    double   strength; // the signed strength times its envelope at this step (the force's st.w)
    double   colour;   // the cloud value it mixes toward
    uint64_t t0;       // the step it starts
    double   dur;      // its life in steps (1500 + 900 h: not whole)
    int      slot;     // its place in the outbreak (0..3), the shader's st[slot]
    double   edge;     // the band edge it broke out on, cells
};
struct SEye {
    double x, y, r, envelope;
};

// The outbreaks and eyes of sketchJove's frame loop, step for step, on a cellsW x cellsH
// lattice with n jets; no GL (unit tested). Two event counters (storms, eyes) start at a
// base drawn from the seed (seed 0: both 0, the sketch's own sequence); the first outbreak
// is at step 500, the first eye at 2500.
class CJupiterSchedule {
  public:
    CJupiterSchedule(int cellsW, int cellsH, uint64_t seed);
    void                   restart();             // from step 0 with the same counter bases
    void                   reseed(uint64_t seed); // new counter bases from `seed` (as the constructor), then restart()
    void                   advanceTo(uint64_t step); // runs every step up to `step` (never back)
    std::span<const SStorm> storms() const { return m_active; } // at most 4 active
    SEye                   eye() const { return m_eye; }
    int                    jets() const { return m_n; }
    uint64_t               stormBase() const { return m_stormBase; }
    uint64_t               eyeBase() const { return m_eyeBase; }
    uint64_t               outbreaks() const { return m_ev; }
    uint64_t               eyes() const { return m_en; }
    uint64_t               lastOutbreakAt() const { return m_lastOutbreak; }
    static int             jetsFor(int cellsH);

  private:
    struct SQ {
        double x, y, r, s, c, t0, dur, edge;
    };
    void stepOnce(uint64_t step);

    int                 m_w, m_h, m_n;
    uint64_t            m_stormBase = 0, m_eyeBase = 0;
    uint64_t            m_at = 0; // the next step to run
    uint64_t            m_ev = 0, m_en = 0, m_lastOutbreak = 0;
    double              m_next = 500, m_e0 = 2500, m_ex = 0, m_ey = 0;
    std::vector<SQ>     m_storms;
    std::vector<SStorm> m_active;
    SEye                m_eye{0, 0, 0, 0};
};

class CJupiterLook final : public ILook {
  public:
    static constexpr float  SPS                 = 360.0f; // 6 steps per frame at the sketch's 60
    static constexpr double BACK                = 2.5e-4; // the cloud's pull back to its bands, per step
    static constexpr int    MAX_TRANSPORT_STEPS = 64;
    static constexpr int    K                   = 4; // cloud cells per lattice cell at quality 1

    // sketchPath: the sketch's per-step cloud in R32F (hyprns-bench --force-sketch-path)
    explicit CJupiterLook(uint64_t seed, bool sketchPath = false);
    ~CJupiterLook() override;
    CJupiterLook(const CJupiterLook&)            = delete;
    CJupiterLook& operator=(const CJupiterLook&) = delete;

    bool        build(const SGeometry& g, float quality, std::string* error) override;
    float       stepsPerSecond() const override;
    void        advance(int steps) override;
    void        draw(GLuint fbo, int x, int y, int w, int h, const std::array<GLfloat, 9>& proj, float fade) override;
    void        checkHealth() override;
    EHealth     takeHealth(bool wait) override;
    void        reset() override;
    std::string status() const override;
    bool        settled() const override { return m_seeded; } // the first cloud is made
    float       meanDensity() override { return m_fluid.stats().rhoMean; }
    SLookInfo   info() const override;
    void        setFrameRate(float fps) override;
    void        setWallClock(uint64_t ns) override;

    // A per-step mix factor a over a transport of n steps: 1 - (1 - a)^n (a itself for n 1).
    static double stepFactor(double a, int n);
    static double backN(int n) { return stepFactor(BACK, n); }
    // The grain's tick: whole 24ths of a second of wall time (64-bit, exact for centuries),
    // and its 12-bit frame value (the tick hashed): what the picture's hash takes.
    static uint64_t grainTick(uint64_t ns);
    static uint32_t grainFrame(uint64_t tick);
    // The cloud's size for a w x h lattice at quality q: round(K w q) x round(K h q), each >= 1.
    static std::array<int, 2> cloudSize(int w, int h, float quality);

    // The extremes the health checks saw since the last call (checks: how many), then
    // starts a new span; for long-run logs (the bench's --status-every).
    struct SSpan {
        int   checks = 0;
        float umax = 0, rhoMin = INFINITY, rhoMax = -INFINITY, rhoMeanMin = INFINITY, rhoMeanMax = -INFINITY;
    };
    SSpan takeSpan();

  private:
    void release();
    void releaseCloud();
    bool makeCloud(int cw, int ch, std::string* error);
    void initialise();
    void setForce();
    void transport(int n);
    void syncColours();
    void packStorms(GLfloat st[16]) const;

    uint64_t m_seed;
    bool     m_sketch;

    int      m_w = 0, m_h = 0;   // lattice
    int      m_cw = 0, m_ch = 0; // cloud
    int      m_pw = 0, m_ph = 0; // the picture (monitor x quality), monitor orientation
    int      m_monW = 0, m_monH = 0;
    float    m_fps    = 30.0f;
    uint64_t m_wallNs = 0;

    CFluid           m_fluid;
    CJupiterSchedule m_sched;
    GLuint           m_cloud[2] = {}, m_cloudFb[2] = {}, m_hat = 0, m_hatFb = 0, m_bar = 0, m_barFb = 0;
    int              m_cur      = 0;
    Gl::SProgram     m_adv, m_mac, m_seedProg, m_resample, m_picture;

    struct {
        GLint st = -1, eye = -1, jets = -1;
    } m_forceU;
    // the cloud's geometry on each cloud program (adv, mac, seed, resample), set by makeCloud
    struct SCloudU {
        GLint size = -1, lsize = -1, k = -1;
    } m_cloudU[4];
    struct {
        GLint dir = -1, frame = -1;
    } m_advU;
    struct {
        GLint st = -1, sc = -1, backN = -1, eye = -1, nst = -1, frame = -1;
    } m_macU;
    struct {
        GLint size = -1, view = -1, mon = -1, grain = -1, fade = -1;
    } m_picU;

    float m_sc[4] = {0, 0, 0, 0}; // the colour per storm slot, kept as the sketch's sc

    bool        m_needInit = true, m_seeded = false;
    uint64_t    m_steps = 0, m_sinceInit = 0, m_resets = 0, m_transports = 0;
    std::string m_resampled; // how the last quality change carried the cloud over

    float    m_umax = 0, m_umean = 0, m_umaxLate = 0; // m_umaxLate: the largest |u| a check saw after step 3000
    float    m_rhoMin = 1, m_rhoMinLate = 1;           // the smallest density at the last check, and after step 3000
    float    m_rhoMax = 1, m_rhoMean = 1;              // the largest and the mean density at the last check
    SSpan    m_span;
    CReduction m_health;
    uint64_t m_checkedAt   = ~uint64_t{0};
    bool     m_lastHealthy = true;
};
