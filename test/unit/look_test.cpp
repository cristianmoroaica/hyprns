#include "check.hpp"
#include "core/Look.hpp"
#include "looks/Pattern.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

// time_scale applies to pattern (60 fluid steps per second), embers (480) and jupiter (360).
// The constructors make no GL calls, stepsPerSecond() neither.
TEST(clock_rate_by_look) {
    const auto pattern = makeLook("pattern", 1);
    const auto embers  = makeLook("embers", 1);
    const auto jupiter = makeLook("jupiter", 1);
    CHECK(pattern && embers && jupiter);
    CHECK(clockRate(*pattern, 2.0f) == 120.0f);
    CHECK(clockRate(*pattern, 0.0f) == 0.0f);
    CHECK(clockRate(*embers, 2.0f) == 960.0f);
    CHECK(clockRate(*jupiter, 2.0f) == 720.0f);
    CHECK(clockRate(*jupiter, 0.0f) == 0.0f);
}

// The look is defined per cell: a stirrer's speed is the same on every lattice (only its
// orbit centre is placed proportionally along the long side).
TEST(pattern_stirrers_in_cells) {
    const CPatternLook look(seedFor("bench"));
    double             vmax[3]    = {0, 0, 0};
    const int          dims[3][2] = {{400, 225}, {538, 225}, {225, 400}};
    for (int t = 0; t < 400000; t += 3)
        for (size_t i = 0; i < 3; i++)
            for (int l = 0; l < 3; l++) {
                const auto s = look.stirrer(i, t, dims[l][0], dims[l][1]);
                vmax[l]      = std::max(vmax[l], std::hypot(s.vx, s.vy));
            }
    std::printf("pattern stirrers: max speed %.6f / %.6f / %.6f cells per fluid step on 400x225 / 538x225 / 225x400\n", vmax[0], vmax[1], vmax[2]);
    CHECK(vmax[0] == vmax[1] && vmax[1] == vmax[2]);
    // the centre moves by 100 * (538 / 400 - 1) cells on 538x225; nothing else changes
    const auto a = look.stirrer(0, 1000, 400, 225), b = look.stirrer(0, 1000, 538, 225), c = look.stirrer(0, 1000, 225, 400);
    CHECK(std::abs((b.x - a.x) - 100.0 * (538.0 / 400.0 - 1.0)) < 1e-9 && b.y == a.y && b.vx == a.vx && b.vy == a.vy);
    // portrait: the sketch's long axis along y
    CHECK(c.y == a.x && c.x == a.y && c.vy == a.vx && c.vx == a.vy);
}
