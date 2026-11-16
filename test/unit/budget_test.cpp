#include "check.hpp"
#include "core/Budget.hpp"
#include "gl/Gl.hpp"

TEST(budget_steps_down_then_up) {
    CBudget b(2.0f, 1.0f); double t = 0;
    auto s = b.update(5.0f, t);            CHECK(s.quality == 0.75f);   // first decision is immediate
    s = b.update(5.0f, t += 5);            CHECK(s.quality == 0.75f);   // too soon
    s = b.update(3.0f, t += 6);            CHECK(s.quality == 0.5f);
    s = b.update(1.9f, t += 11);           CHECK(s.quality == 0.5f);    // inside the dead band
    s = b.update(0.6f, t += 11);           CHECK(s.quality == 0.75f);   // 0.6 x 2.25 = 1.35 < 1.6
}
TEST(budget_second_lever) {
    CBudget b(2.0f, 1.0f); double t = 0; SBudgetState s;
    for (int i = 0; i < 3; i++) s = b.update(9.0f, t += 11);
    CHECK(s.quality == 0.35f && s.timeScale == 1.0f && !s.overBudget);
    for (int i = 0; i < 3; i++) s = b.update(9.0f, t += 11);
    CHECK(s.timeScale == 0.5f);
    s = b.update(9.0f, t += 11);           CHECK(s.overBudget);
}
TEST(budget_off_or_unmeasured) {
    CBudget off(0.0f, 0.5f);  CHECK(off.update(9.0f, 0).quality == 0.5f);
    CBudget b(2.0f, 0.75f);   auto s = b.update(-1.0f, 0);
    CHECK(s.quality == 0.75f && !s.measured && s.timeScale == 1.0f);
}

// Beyond the brief: the plugin feeds -1 while its mean refills after a level change;
// that holds the levels instead of jumping back to the setting.
TEST(budget_holds_while_the_mean_refills) {
    CBudget b(2.0f, 1.0f);
    auto s = b.update(5.0f, 0);            CHECK(s.quality == 0.75f && s.measured);
    s = b.update(-1.0f, 20);               CHECK(s.quality == 0.75f && s.measured);
}
// The quality setting is the start and the ceiling; time recovers before quality.
TEST(budget_ceiling_and_recovery_order) {
    CBudget b(2.0f, 0.5f); double t = 0;
    auto s = b.update(0.1f, t);            CHECK(s.quality == 0.5f);    // never above the setting
    s = b.update(9.0f, t += 11);           CHECK(s.quality == 0.35f);
    s = b.update(9.0f, t += 11);           CHECK(s.quality == 0.35f && s.timeScale == 0.85f);
    s = b.update(0.5f, t += 11);           CHECK(s.quality == 0.35f && s.timeScale == 1.0f);
    s = b.update(0.5f, t += 11);           CHECK(s.quality == 0.5f && s.timeScale == 1.0f);
    s = b.update(0.1f, t += 11);           CHECK(s.quality == 0.5f);
}
TEST(budget_over_clears) {
    CBudget b(2.0f, 0.35f); double t = 0; SBudgetState s;
    for (int i = 0; i < 4; i++) s = b.update(9.0f, t += 11);
    CHECK(s.timeScale == 0.5f && s.overBudget);
    s = b.update(2.0f, t += 1);            CHECK(!s.overBudget && s.timeScale == 0.5f); // in the band, too soon to move
}
// Fix round 1: after a restart nothing is reported until the warm-up and a fresh window
// are in, and no sample from before the restart ever counts.
TEST(gpu_mean_restart_forgets) {
    CGpuMean m;
    for (int i = 0; i < CGpuMean::WARMUP + CGpuMean::WINDOW + 7; i++) m.add(50.0f);
    CHECK_NEAR(m.mean(), 50.0, 1e-5);
    m.restart();
    for (int i = 0; i < CGpuMean::WARMUP; i++) m.add(1000.0f);      // the ramp after idle: dropped
    for (int i = 0; i < CGpuMean::WINDOW - 1; i++) { m.add(2.0f); CHECK(m.mean() < 0); }
    m.add(2.0f);                           CHECK_NEAR(m.mean(), 2.0, 1e-6);
}
// A monitor dark for longer than DARK_SECONDS resumes with a fresh mean.
TEST(dark_gap) {
    CHECK(!resumedAfterDark(-1.0, 100.0));    // the first frame
    CHECK(!resumedAfterDark(10.0, 10.0 + 1.0 / 30));
    CHECK(!resumedAfterDark(10.0, 10.0 + DARK_SECONDS));
    CHECK(resumedAfterDark(10.0, 10.0 + DARK_SECONDS + 0.01));
    CHECK(resumedAfterDark(10.0, 400.0));
}
// A timer result counts only when no discard() and no disjoint event came after its begin.
TEST(timer_keep_result) {
    CHECK(Gl::keepResult(3, 3, 7, 7));
    CHECK(!Gl::keepResult(3, 4, 7, 7));       // discarded since
    CHECK(!Gl::keepResult(3, 3, 7, 8));       // a disjoint event seen by any timer since
}
TEST(gpu_mean_window) {
    CGpuMean m;
    CHECK(m.mean() < 0 && m.last() < 0);
    for (int i = 0; i < CGpuMean::WARMUP; i++) m.add(100.0f);       // dropped
    for (int i = 0; i < CGpuMean::WINDOW - 1; i++) m.add(1.0f);
    CHECK(m.mean() < 0);
    m.add(1.0f);                           CHECK_NEAR(m.mean(), 1.0, 1e-6);
    m.add(31.0f);                          CHECK_NEAR(m.mean(), 2.0, 1e-5);  // rolling: one of 30 replaced
    m.restart();                           CHECK(m.mean() < 0);
    CHECK_NEAR(m.last(), 2.0, 1e-5);       // the last full mean stays for the status
}
