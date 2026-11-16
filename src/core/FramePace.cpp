#include "core/FramePace.hpp"

#include "core/Budget.hpp"

#include <algorithm>
#include <cmath>

float fadeAfter(float fade, double dt, bool out) {
    const double next = std::clamp(static_cast<double>(fade) + (out ? -dt : dt) / FADE_SECONDS, 0.0, 1.0);
    if (next < 1e-4)
        return 0.0f;
    if (next > 1.0 - 1e-4)
        return 1.0f;
    return static_cast<float>(next);
}

double fadeDt(double lastAdvance, double now, int fps) {
    if (lastAdvance < 0)
        return 0.0;
    return std::clamp(now - lastAdvance, 0.0, std::max(0.1, 2.0 / std::max(1, fps)));
}

bool blurRefreshDue(int blurFps, int fps, double now, double lastRefresh) {
    if (blurFps < 0)
        return true;
    if (blurFps == 0)
        return false;
    if (lastRefresh < 0)
        return true;
    const int frameRate = std::max(1, fps);
    const int rate      = std::min(blurFps, frameRate);
    // k frames between refreshes; half a frame of slack so a frame k intervals on counts,
    // but never closer than 1/rate (less 10 us: the timer counts whole microseconds)
    const int    k   = (frameRate + rate - 1) / rate;
    const double gap = now - lastRefresh;
    return gap >= (k - 0.5) / frameRate && gap >= 1.0 / rate - 1e-5;
}

bool pictureFrozen(float timeScale, float pace, float fade, bool switching, bool resetWaiting, bool settled) {
    return (timeScale <= 0.0f || pace <= 0.0f) && fade >= 1.0f && !switching && !resetWaiting && settled;
}

bool CDarkLatch::see(double lastFrame, double now) {
    const bool dark = resumedAfterDark(lastFrame, now);
    m_pending       = m_pending || dark;
    return dark;
}

SFrameDecision decideFrame(const SFrameInputs& in) {
    SFrameDecision d;
    if (in.failed)
        return d;
    if (!in.visible) {
        d.dark = true;
        return d;
    }
    if (!in.hasLook) {
        d.frame = d.blur = true;
        return d;
    }
    d.frozen = pictureFrozen(in.timeScale, in.pace, in.fade, in.switching, in.resetWaiting, in.settled);
    if (d.frozen) {
        d.dark  = true;
        d.frame = !in.wasFrozen || in.wallClock;
        d.blur  = !in.wasFrozen;
        return d;
    }
    d.frame = true;
    d.blur  = blurRefreshDue(in.blurFps, in.fps, in.now, in.lastBlurRefresh);
    return d;
}
