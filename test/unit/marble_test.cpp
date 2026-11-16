#include "check.hpp"
#include "core/Look.hpp"
#include "looks/Marble.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

double sketchSmoothstep(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

} // namespace

// The vein families and the brass at a step, as the sketch's pShow computes them from
// `time` (LIFE 7200, PER, the 0.37 k offsets, sd = floor(ph) 17.31 + 41.7 k, brass
// epochs of 14400 steps offset 9.1 n), while the seeds are small enough to be the
// sketch's own; reduced below 509 later, so float holds them on the GPU after days.
TEST(marble_stone_time_is_the_sketch) {
    const double PER[4] = {1.0, 1.27, 1.618, 0.81};
    for (const double step : {0.0, 1.0, 2400.0, 7199.0, 7200.0, 9000.0, 30000.0, 86399.0}) {
        const auto st = CMarbleLook::stoneAt(step);
        for (int k = 0; k < 4; k++) {
            const double ph = step / (7200.0 * PER[k]) + 0.37 * k, f = ph - std::floor(ph);
            CHECK_NEAR(st.life[k], sketchSmoothstep(0.0, 0.3, f) * sketchSmoothstep(1.0, 0.7, f), 1e-6);
            CHECK_NEAR(st.seed[k], std::floor(ph) * 17.31 + 41.7 * k, 1e-3);
        }
        const double bph = step / 14400.0, bn = std::floor(bph);
        CHECK_NEAR(st.brass[0], bn * 9.1, 1e-3);
        CHECK_NEAR(st.brass[1], (bn + 1) * 9.1, 1e-3);
        CHECK_NEAR(st.brass[2], sketchSmoothstep(0.0, 1.0, bph - bn), 1e-6);
    }
    // a year at 120 steps per second: every offset stays small, life stays in 0..1
    for (const double step : {3.784e9, 3.784e9 + 3600.0, 1e12}) {
        const auto st = CMarbleLook::stoneAt(step);
        for (int k = 0; k < 4; k++)
            CHECK(st.seed[k] >= 0.0f && st.seed[k] < 509.0f && st.life[k] >= 0.0f && st.life[k] <= 1.0f);
        CHECK(st.brass[0] >= 0.0f && st.brass[0] < 509.0f && st.brass[1] >= 0.0f && st.brass[1] < 509.0f);
    }
}

// The look is defined per cell: a stirrer's speed and orbit are the same on every
// lattice; only its orbit centre is placed proportionally along the long side.
TEST(marble_stirrers_in_cells) {
    const CMarbleLook look(seedFor("bench"));
    double            vmax[3]    = {0, 0, 0};
    const int         dims[3][2] = {{400, 225}, {538, 225}, {225, 400}};
    for (int t = 0; t < 400000; t += 3)
        for (size_t i = 0; i < 3; i++)
            for (int l = 0; l < 3; l++) {
                const auto s = look.stirrer(i, t, dims[l][0], dims[l][1]);
                vmax[l]      = std::max(vmax[l], std::hypot(s.vx, s.vy));
            }
    std::printf("marble stirrers: max speed %.6f / %.6f / %.6f cells per fluid step on 400x225 / 538x225 / 225x400\n", vmax[0], vmax[1], vmax[2]);
    CHECK(vmax[0] == vmax[1] && vmax[1] == vmax[2]);
    const auto a = look.stirrer(0, 1000, 400, 225), b = look.stirrer(0, 1000, 538, 225), c = look.stirrer(0, 1000, 225, 400);
    CHECK(std::abs((b.x - a.x) - 100.0 * (538.0 / 400.0 - 1.0)) < 1e-9 && b.y == a.y && b.vx == a.vx && b.vy == a.vy);
    CHECK(c.y == a.x && c.x == a.y && c.vy == a.vx && c.vx == a.vy);
}

TEST(marble_is_registered) {
    const auto marble = makeLook("marble", 1);
    CHECK(marble && marble->stepsPerSecond() == 120.0f);
    CHECK(clockRate(*marble, 0.5f) == 60.0f);
    CHECK(dynamic_cast<CMarbleLook*>(marble.get()) != nullptr);
}
