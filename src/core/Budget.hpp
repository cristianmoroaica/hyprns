#pragma once

// The GPU time budget of one monitor: lowers the picture's quality (the resolution the
// look draws at), then the simulation's pace, while the measured GPU time per frame is
// over budget, and raises them again when there is room. Pure C++ (no GL): the caller
// measures and passes its running mean (CGpuMean).

#include <array>

struct SBudgetState {
    float quality    = 1;
    float timeScale  = 1;
    bool  overBudget = false; // at the lowest quality and pace and still over
    bool  measured   = false; // false: the levels are the quality setting and pace 1
};

// Rules: levels 1, 0.75, 0.5, 0.35 (quality) and 1, 0.85, 0.7, 0.5 (pace). A move of
// either lever at most every 10 s; the first one at once. Down when the mean is above
// 1.1 x budget: quality first, then pace. Up when the projected cost is below 0.8 x
// budget: pace first (cost taken as proportional to pace), then quality (cost taken as
// proportional to its square: the quality is the picture's resolution). The quality
// setting (fixedQuality) is the starting level and the ceiling. budgetMs <= 0: the
// controller is off. A negative mean (no measurement) before the first measurement
// gives the setting; after one, it holds the levels (the caller's mean refills after a
// level change). The budget counts the plugin's own passes only: a blur Hyprland redoes
// because of the plugin's frames (main.cpp, requestFrame; blur_fps) is not in it.
class CBudget {
  public:
    CBudget(float budgetMs, float fixedQuality);
    SBudgetState update(float gpuMsPerFrame, double nowSeconds); // gpuMsPerFrame < 0 = no measurement

    static constexpr std::array<float, 4> QUALITY = {1.0f, 0.75f, 0.5f, 0.35f};
    static constexpr std::array<float, 4> PACE    = {1.0f, 0.85f, 0.7f, 0.5f};
    static constexpr double               MOVE_SECONDS = 10.0;

    float budgetMs() const { return m_budget; }
    float fixedQuality() const { return m_fixed; }

  private:
    SBudgetState state() const;

    float  m_budget, m_fixed;
    int    m_ceiling = 0, m_q = 0, m_t = 0; // indices into QUALITY / PACE
    bool   m_measured = false, m_moved = false, m_over = false;
    double m_lastMove = 0.0;
};

// A frame this long after the last one ends a dark spell (monitor off, fullscreen
// window): the caller restarts its mean, so the first decision after it uses neither
// the window from before nor the slow first frames (the warm-up drops those).
inline constexpr double DARK_SECONDS = 1.0;
inline bool             resumedAfterDark(double lastFrame, double now) {
    return lastFrame >= 0.0 && now - lastFrame > DARK_SECONDS;
}

// The running mean of GPU ms per frame fed to CBudget. After restart() (a level change,
// a new look) it drops WARMUP samples (the first frames of a new target or lattice
// touch fresh memory), then answers -1 until WINDOW samples are in; from then on the
// mean of the last WINDOW. WINDOW 30: one second at the default 30 fps, enough to
// smooth frame-to-frame noise and far shorter than the 10 s between moves, so every
// decision sees only frames drawn at the current levels.
class CGpuMean {
  public:
    static constexpr int WARMUP = 3;
    static constexpr int WINDOW = 30;

    void  restart();
    void  add(float ms);
    float mean() const; // -1 until the window is full
    float last() const; // the last full-window mean, kept across restart(); -1 none yet

  private:
    std::array<float, WINDOW> m_ring{};
    int                       m_skip = WARMUP, m_count = 0, m_next = 0;
    double                    m_sum  = 0.0;
    float                     m_last = -1.0f;
};
