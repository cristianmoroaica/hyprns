#include "core/StepClock.hpp"

#include <algorithm>
#include <cmath>

double CStepClock::maxDtFor(int fps, bool afterDark) {
    return afterDark ? std::min(MAX_DT, 2.0 / std::max(1, fps)) : MAX_DT;
}

int CStepClock::tick(double nowSeconds, float stepsPerSecond, double maxDt) {
    if (!m_started) {
        m_started = true;
        m_last    = nowSeconds;
        return 0;
    }
    const double dt = std::clamp(nowSeconds - m_last, 0.0, std::clamp(maxDt, 0.0, MAX_DT));
    m_last          = nowSeconds;
    if (!(stepsPerSecond > 0.0f)) {
        m_debt = 0.0;
        return 0;
    }
    // a tiny epsilon: 1/30 s at 360 steps/s is 11.999999999 in doubles, which is 12
    m_debt += dt * stepsPerSecond;
    const int steps = std::min(static_cast<int>(m_debt + 1e-9), MAX_STEPS);
    m_debt          = std::max(0.0, m_debt - steps);
    // what a capped frame could not run is dropped, not owed to the next ones
    if (steps == MAX_STEPS)
        m_debt -= std::floor(m_debt);
    return steps;
}
