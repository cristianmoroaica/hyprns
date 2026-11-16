#include "check.hpp"
#include "core/Budget.hpp"
#include "core/StepClock.hpp"

TEST(clock) {
    CStepClock c; c.tick(0.0, 360);
    CHECK(c.tick(1.0 / 30, 360) == 12);
    int total = 0; for (int i = 2; i <= 31; i++) total += c.tick(i / 30.0, 360);
    CHECK(total == 360);                        // a second is 360 steps at any frame rate
    CHECK(c.tick(600.0, 360) <= 64);            // ten minutes dark: no burst
    CHECK(c.tick(600.0 + 1.0 / 30, 0) == 0);    // time_scale 0 freezes
}
// The plugin's policy (CStepClock::maxDtFor with resumedAfterDark): fps 240 asked on a
// 60 Hz monitor, so redraws come every 1/60 s, four fps intervals. A second is still
// 360 steps (a cap of two fps intervals on every frame would give 180); a wake after ten
// minutes dark runs two fps intervals' steps.
TEST(clock_pace_at_slow_redraws) {
    const int fps  = 240;
    double    last = -1.0;
    CStepClock c;
    const auto frame = [&](double now) {
        const int steps = c.tick(now, 360, CStepClock::maxDtFor(fps, resumedAfterDark(last, now)));
        last            = now;
        return steps;
    };
    frame(0.0);
    int total = 0;
    for (int i = 1; i <= 60; i++)
        total += frame(i / 60.0);
    CHECK(total == 360);
    CHECK(frame(600.0) == 3);                   // ten minutes dark: 2/240 s of steps
    total = 0;
    for (int i = 1; i <= 60; i++)
        total += frame(600.0 + i / 60.0);
    CHECK(total == 360);                        // and the pace is back at once
}
// A cap of two frame intervals: a wake from dark runs two frames' steps, and a steady
// frame rate is untouched (a second is still 360 steps).
TEST(clock_catch_up_cap) {
    const double cap = 2.0 / 30;
    CStepClock   c;
    c.tick(0.0, 360, cap);
    int total = 0;
    for (int i = 1; i <= 30; i++)
        total += c.tick(i / 30.0, 360, cap);
    CHECK(total == 360);
    CHECK(c.tick(600.0, 360, cap) == 24);        // ten minutes dark: two frames' steps
    CHECK(c.tick(600.0 + 1.0 / 30, 360, cap) == 12);
}
