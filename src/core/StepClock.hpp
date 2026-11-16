#pragma once

// Turns wall-clock time into a whole number of simulation steps per frame. The first
// tick only sets the reference time. Fractions carry over, so a second is the same
// number of steps at any frame rate; a frame's time counts at most maxDt (at most
// MAX_DT, 0.25 s) and a frame runs at most 64 steps, so a long pause (monitor off)
// resumes without a burst. The plugin passes maxDtFor(fps, resumed): two frame
// intervals on the first frame after a dark spell, which then costs about two normal
// frames, not 64 steps' worth (110 ms on an integrated GPU); MAX_DT on every other frame,
// so redraws that come less often than fps asks (fps above the monitor's refresh, a busy
// compositor) keep the wall-clock pace.
// A rate <= 0 freezes the look and drops the carried fraction.
class CStepClock {
  public:
    int tick(double nowSeconds, float stepsPerSecond, double maxDt = MAX_DT);
    // the cap for a frame: two intervals of fps on the first frame after a dark spell
    // (resumedAfterDark), MAX_DT otherwise
    static double maxDtFor(int fps, bool afterDark);

    static constexpr double MAX_DT    = 0.25;
    static constexpr int    MAX_STEPS = 64;

  private:
    bool   m_started = false;
    double m_last    = 0.0;
    double m_debt    = 0.0;
};
