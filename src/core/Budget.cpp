#include "core/Budget.hpp"

#include <cmath>

CBudget::CBudget(float budgetMs, float fixedQuality) : m_budget(budgetMs), m_fixed(fixedQuality) {
    // the level nearest the setting (Settings only lets the four through)
    for (int i = 1; i < static_cast<int>(QUALITY.size()); i++)
        if (std::fabs(QUALITY[i] - fixedQuality) < std::fabs(QUALITY[m_ceiling] - fixedQuality))
            m_ceiling = i;
    m_q = m_ceiling;
}

SBudgetState CBudget::state() const {
    if (!m_measured)
        return {m_fixed, 1.0f, false, false};
    return {QUALITY[m_q], PACE[m_t], m_over, true};
}

SBudgetState CBudget::update(float gpuMs, double now) {
    if (!(m_budget > 0.0f))
        return {m_fixed, 1.0f, false, false};
    if (!(gpuMs >= 0.0f))
        return state(); // the setting before the first measurement, else the levels held
    m_measured = true;

    const bool canMove = !m_moved || now - m_lastMove >= MOVE_SECONDS;
    const auto move    = [&](int& level, int by) {
        level += by;
        m_moved    = true;
        m_lastMove = now;
    };
    const int last = static_cast<int>(QUALITY.size()) - 1;

    if (gpuMs > 1.1f * m_budget) {
        if (m_q < last) {
            if (canMove)
                move(m_q, +1);
        } else if (m_t < last) {
            if (canMove)
                move(m_t, +1);
        } else
            m_over = true;
        return state();
    }

    m_over = false;
    if (!canMove)
        return state();
    if (m_t > 0) {
        if (gpuMs * (PACE[m_t - 1] / PACE[m_t]) < 0.8f * m_budget)
            move(m_t, -1);
    } else if (m_q > m_ceiling) {
        const float r = QUALITY[m_q - 1] / QUALITY[m_q];
        if (gpuMs * (r * r) < 0.8f * m_budget)
            move(m_q, -1);
    }
    return state();
}

void CGpuMean::restart() {
    m_skip  = WARMUP;
    m_count = 0;
    m_next  = 0;
    m_sum   = 0.0;
}

void CGpuMean::add(float ms) {
    if (m_skip > 0) {
        m_skip--;
        return;
    }
    if (m_count < WINDOW)
        m_count++;
    m_ring[m_next] = ms;
    m_next         = (m_next + 1) % WINDOW;
    // summed afresh (30 adds): a running sum would drift over days of frames
    m_sum = 0.0;
    for (int i = 0; i < m_count; i++)
        m_sum += m_ring[i];
    if (m_count == WINDOW)
        m_last = static_cast<float>(m_sum / WINDOW);
}

float CGpuMean::mean() const {
    return m_count == WINDOW ? static_cast<float>(m_sum / WINDOW) : -1.0f;
}

float CGpuMean::last() const {
    return m_last;
}
