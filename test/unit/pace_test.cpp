#include "check.hpp"
#include "core/FramePace.hpp"
#include "core/Settings.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

TEST(fade_seconds) {
    // 0.4 s each way: the owner asked for 0.4 s (goal.md, 2026-11-16); the spec's wording
    // can also mean 0.4 s in total
    CHECK(FADE_SECONDS == 0.4);
    // 0.4 s at 30 fps: 12 frames from 1 to 0, 12 back
    float fade = 1.0f;
    int   out  = 0;
    while (fade > 0.0f && out < 100) {
        fade = fadeAfter(fade, 1.0 / 30.0, true);
        out++;
    }
    CHECK(out == 12);
    int in = 0;
    while (fade < 1.0f && in < 100) {
        fade = fadeAfter(fade, 1.0 / 30.0, false);
        in++;
    }
    CHECK(in == 12);
    CHECK(fadeAfter(0.5f, 0.0, true) == 0.5f);
    CHECK(fadeAfter(0.1f, 1.0, true) == 0.0f && fadeAfter(0.9f, 1.0, false) == 1.0f);
}

TEST(blur_refresh) {
    // -1 (the default): every wallpaper frame
    CHECK(SSettings{}.blurFps == -1);
    CHECK(blurRefreshDue(-1, 30, 10.0, 10.0));
    // 0: never from the timer
    CHECK(!blurRefreshDue(0, 30, 10.0, -1.0));
    CHECK(!blurRefreshDue(0, 30, 100.0, 10.0));
    // N: the first frame, then at most N a second
    CHECK(blurRefreshDue(10, 30, 5.0, -1.0));
    CHECK(!blurRefreshDue(10, 30, 5.0 + 1.0 / 30.0, 5.0));
    CHECK(!blurRefreshDue(10, 30, 5.0 + 2.0 / 30.0, 5.0));
    CHECK(blurRefreshDue(10, 30, 5.0 + 3.0 / 30.0, 5.0));
    // the timer's interval is whole microseconds (33333 at 30 fps): still on time
    CHECK(blurRefreshDue(10, 30, 5.0 + 3 * 0.033333, 5.0));
    // N at or above fps: every frame
    CHECK(blurRefreshDue(30, 30, 5.0 + 0.033333, 5.0));
    CHECK(blurRefreshDue(240, 30, 5.0 + 0.033333, 5.0));
    CHECK(!blurRefreshDue(30, 30, 5.0 + 0.030, 5.0));
    // over 3 s of 30 fps frames, blur_fps 10 refreshes 30 times (+1 for the first)
    double last = -1.0;
    int    n    = 0;
    for (int frame = 0; frame <= 90; frame++) {
        const double now = 100.0 + frame / 30.0 + ((frame % 3) - 1) * 0.002; // jitter
        if (blurRefreshDue(10, 30, now, last)) {
            last = now;
            n++;
        }
    }
    CHECK(n == 31);
}

TEST(blur_fps_setting) {
    SSettings s;
    CHECK(applySetting(s, "blur_fps", "0").empty() && s.blurFps == 0);
    CHECK(applySetting(s, "blur_fps", "15").empty() && s.blurFps == 15);
    CHECK(applySetting(s, "blur_fps", "-1").empty() && s.blurFps == -1);
    CHECK(!applySetting(s, "blur_fps", "-2").empty() && s.blurFps == -1);
    CHECK(!applySetting(s, "blur_fps", "2.5").empty());
    CHECK(!applySetting(s, "blur_fps", "241").empty());
    CHECK(describeSettings(SSettings{}).find("blur_fps -1\n") != std::string::npos);
}

TEST(picture_frozen) {
    // pace 0 (time_scale 0, or the budget's pace) with nothing else going on
    CHECK(pictureFrozen(0.0f, 1.0f, 1.0f, false, false, true));
    CHECK(pictureFrozen(1.0f, 0.0f, 1.0f, false, false, true));
    // moving, fading, switching, about to reset or still baking: not frozen
    CHECK(!pictureFrozen(1.0f, 1.0f, 1.0f, false, false, true));
    CHECK(!pictureFrozen(0.0f, 1.0f, 0.5f, false, false, true));
    CHECK(!pictureFrozen(0.0f, 1.0f, 1.0f, true, false, true));
    CHECK(!pictureFrozen(0.0f, 1.0f, 1.0f, false, true, true));
    CHECK(!pictureFrozen(0.0f, 1.0f, 1.0f, false, false, false));
}

TEST(advance_gate) {
    CAdvanceGate gate;
    // a redraw Hyprland made for its own reasons does not move the picture on
    CHECK(!gate.take(false));
    // the timer's frame does, once
    gate.request();
    CHECK(gate.take(false));
    CHECK(!gate.take(false));
    // a request waits for the next draw, however many timer ticks came before it
    gate.request();
    gate.request();
    CHECK(gate.take(false));
    CHECK(!gate.take(false));
    // a look just built, rebuilt or reset always takes its first step
    CHECK(gate.take(true));
}

// I2: blur_fps N never refreshes more than N times a second, also when N does not divide
// fps and the timer fires late (it is re-armed from its own callback: never early).
TEST(blur_refresh_cap) {
    for (const int fps : {30, 60, 144}) {
        for (const int n : {7, 12, 17, 20, 25, 40, 45, 59, 60, 100}) {
            for (const double late : {0.0, 0.0005, 0.002, -1.0}) { // -1: 0..3 ms, varying
                double   now = 50.0, last = -1.0, minGap = 1e9;
                int      count = 0;
                unsigned lcg   = 12345;
                while (now < 60.0) {
                    if (blurRefreshDue(n, fps, now, last)) {
                        if (last >= 0)
                            minGap = std::min(minGap, now - last);
                        last = now;
                        count++;
                    }
                    lcg = lcg * 1664525u + 1013904223u;
                    // the plugin's timer interval: whole microseconds (main.cpp)
                    now += (1000000 / fps) * 1e-6 + (late >= 0 ? late : (lcg >> 8) % 3000 * 1e-6);
                }
                const int cap = std::min(n, fps);
                CHECK(count <= cap * 10 + 1);
                CHECK(minGap >= 1.0 / cap - 1e-5);
                // and not far below it on time: fps / ceil(fps / N) a second
                if (late == 0.0)
                    CHECK(count >= 10 * fps / ((fps + cap - 1) / cap) - 1);
            }
        }
    }
}

// m1: below 10 fps a fade still takes 0.4 s each way (the frame's dt is not cut to 0.1 s)
TEST(fade_low_fps) {
    for (const int fps : {2, 5, 10, 30, 144}) {
        float fade = 1.0f;
        int   n    = 0;
        double last = 0.0;
        for (double now = 1.0 / fps; fade > 0.0f && n < 1000; now += 1.0 / fps, n++) {
            fade = fadeAfter(fade, fadeDt(last, now, fps), true);
            last = now;
        }
        CHECK(n == static_cast<int>(std::lround(FADE_SECONDS * fps)) || (fps == 2 && n == 1));
    }
    // the first frame, and a long gap (a dark spell), do not jump the fade
    CHECK(fadeDt(-1.0, 5.0, 30) == 0.0);
    CHECK(fadeDt(1.0, 5.0, 30) <= 0.1);
}

// m9, C1, m4: the per-monitor decision of the frame timer
TEST(frame_decision) {
    SFrameInputs in;
    in.hasLook = true;
    in.now     = 10.0;
    // running: a frame, and a blur refresh with blur_fps -1
    auto d = decideFrame(in);
    CHECK(d.frame && d.blur && !d.frozen && !d.dark);
    // no look yet (first frame): a frame and a refresh
    in.hasLook = false;
    in.timeScale = 0.0f;
    d = decideFrame(in);
    CHECK(d.frame && d.blur && !d.frozen);
    in.hasLook = true;
    // a monitor where nothing could be built: nothing
    in.failed = true;
    d = decideFrame(in);
    CHECK(!d.frame && !d.blur);
    in.failed = false;
    // not visible (DPMS off, fullscreen window): no frame, and the next frame that moves
    // the picture catches up as after a dark spell
    in.visible = false;
    in.timeScale = 1.0f;
    d = decideFrame(in);
    CHECK(!d.frame && !d.blur && d.dark);
    in.visible = true;

    // C1: time_scale 0, but the look is still baking its picture: frames go on
    in.timeScale = 0.0f;
    in.settled   = false;
    in.wasFrozen = false;
    d = decideFrame(in);
    CHECK(d.frame && !d.frozen);
    // ...also after a frozen spell (a reset or rebuild while frozen)
    in.wasFrozen = true;
    d = decideFrame(in);
    CHECK(d.frame && !d.frozen);
    // the bake is whole: the first frozen tick still draws once and refreshes the blur
    // (m4: the cache gets the final picture), then nothing
    in.settled   = true;
    in.wasFrozen = false;
    in.blurFps   = 5;
    in.lastBlurRefresh = 9.99;
    d = decideFrame(in);
    CHECK(d.frame && d.blur && d.frozen && d.dark);
    in.wasFrozen = true;
    d = decideFrame(in);
    CHECK(!d.frame && !d.blur && d.frozen && d.dark);
    // a wall-clock look (jupiter's grain) keeps its frames while frozen, without refreshes
    in.wallClock = true;
    d = decideFrame(in);
    CHECK(d.frame && !d.blur && d.frozen);
    in.wallClock = false;
    // a switch or a reset while frozen: frames again
    in.switching = true;
    d = decideFrame(in);
    CHECK(d.frame && !d.frozen);
    in.switching = false;
    in.resetWaiting = true;
    d = decideFrame(in);
    CHECK(d.frame && !d.frozen);
    in.resetWaiting = false;
    // unfrozen: frames, blur at blur_fps
    in.timeScale = 1.0f;
    d = decideFrame(in);
    CHECK(d.frame && !d.blur && !d.frozen && !d.dark);
    in.lastBlurRefresh = 9.7;
    d = decideFrame(in);
    CHECK(d.frame && d.blur);
}

// I1: a dark spell seen by any draw, or marked by the timer, is consumed by the next
// draw that moves the picture on, not by Hyprland's own redraw before it
TEST(dark_latch) {
    CDarkLatch dark;
    CHECK(!dark.take());
    // Hyprland's redraw after 5 s dark sees it (and the caller restarts its mean)...
    CHECK(dark.see(10.0, 15.0));
    // ...a later redraw does not, but the latch holds it
    CHECK(!dark.see(15.0, 15.01));
    CHECK(dark.take());
    CHECK(!dark.take());
    // the timer marks it (a frozen or hidden tick): the next advance takes it
    dark.mark();
    CHECK(!dark.see(15.01, 15.02));
    CHECK(dark.take());
    // the first draw (no last frame) is not dark
    CDarkLatch fresh;
    CHECK(!fresh.see(-1.0, 3.0));
    CHECK(!fresh.take());
}
