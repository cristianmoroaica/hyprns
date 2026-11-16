#include "check.hpp"
#include "core/Look.hpp"
#include "core/Sequence.hpp"
#include "looks/Embers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

double ss(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// The page's frame loop (reference/equivalence.html, sketchEmbers), events only, on the
// sketch's 400x225 lattice, its event counter starting at `base` instead of 0.
struct SPageEvents {
    double base = 0, ev = 0, nextPop = 2500, nextGust = 7000;
    bool   havePop = false, haveGust = false;
    double px = 0, py = 0, pr = 0, gt0 = 0, gcx = 0, gcy = 0;
    int    pn = 0;
    struct SOut {
        double wx, wy;
        bool   pop;
        double x, y, r, w;
    };
    SOut step(double s) {
        const double t  = s;
        double       wx = 2.2e-6 * (std::sin(t * 7.1e-4) + 0.7 * std::sin(t * 7.1e-4 * 1.6180339887 + 1.0)) * ss(0.2, 0.9, std::abs(std::sin(t * 2.3e-4 * 1.4142135623)));
        double       wy = 0;
        if (s >= nextPop) {
            ev++;
            const double i = base + ev;
            nextPop        = s + 3000 + 6000 * frac(i * PHI);
            px             = frac(i * PLASTIC) * 400;
            py             = frac(i * 0.5698402910) * 225;
            pr             = 7 + 6 * frac(i * 0.3819660113);
            pn             = 30;
            havePop        = true;
        }
        if (s >= nextGust) {
            ev++;
            const double i = base + ev;
            nextGust       = s + 12000 + 13000 * frac(i * PHI);
            const double a = i * 2.39996;
            gt0            = s;
            gcx            = std::cos(a);
            gcy            = std::sin(a);
            haveGust       = true;
        }
        if (haveGust) {
            const double dt = s - gt0;
            if (dt > 1800)
                haveGust = false;
            else {
                const double g = 9e-6 * ss(0, 300, dt) * ss(1800, 1300, dt);
                wx += gcx * g;
                wy += gcy * g;
            }
        }
        SOut o{wx, wy, false, 0, 0, 0, 0};
        if (havePop) {
            o = {wx, wy, true, px, py, pr, 1.7};
            if (--pn <= 0)
                havePop = false;
        }
        return o;
    }
};

} // namespace

// The pops and gusts come from the page's schedule step for step (first pop at 2500, first
// gust at 7000, one event counter for both); the seed only moves where the counter starts.
TEST(embers_schedule_is_the_page_from_the_seed) {
    for (const uint64_t base : {uint64_t{0}, uint64_t{12345}, uint64_t{1} << 19}) {
        CEmbersEvents ev(base);
        SPageEvents   page;
        page.base = static_cast<double>(base);
        int pops = 0, gusts = 0, popSteps = 0;
        for (uint64_t s = 0; s < 120000; s++) {
            const auto a = ev.step(s, 400, 225);
            const auto b = page.step(static_cast<double>(s));
            CHECK_NEAR(a.wx, b.wx, 1e-15);
            CHECK_NEAR(a.wy, b.wy, 1e-15);
            CHECK(a.pop == b.pop);
            if (a.pop && b.pop) {
                CHECK_NEAR(a.popX, b.x, 1e-9);
                CHECK_NEAR(a.popY, b.y, 1e-9);
                CHECK_NEAR(a.popR, b.r, 1e-12);
                CHECK(a.popHeat == b.w);
                popSteps++;
            }
        }
        pops  = static_cast<int>(ev.pops());
        gusts = static_cast<int>(ev.gusts());
        std::printf("embers events from base %llu: %d pops (%d steps alive), %d gusts in 120000 steps\n", static_cast<unsigned long long>(base), pops, popSteps, gusts);
        CHECK(pops >= 10 && gusts >= 4 && popSteps == 30 * pops);
    }
    // the first pop at 2500 lives 2500..2529, the first gust starts at 7000
    CEmbersEvents ev(0);
    for (uint64_t s = 0; s < 2500; s++)
        CHECK(!ev.step(s, 400, 225).pop);
    for (uint64_t s = 2500; s < 2530; s++)
        CHECK(ev.step(s, 400, 225).pop);
    CHECK(!ev.step(2530, 400, 225).pop);
    CHECK(ev.gusts() == 0);
    for (uint64_t s = 2531; s <= 7000; s++)
        ev.step(s, 400, 225);
    CHECK(ev.gusts() == 1 && ev.lastGustAt() == 7000);
    // restart: the same schedule again
    ev.restart();
    CHECK(ev.pops() == 0 && ev.gusts() == 0);
    // the look's base comes from its seed: the same seed, the same base; others differ
    CHECK(CEmbersLook(seedFor("DP-4")).eventBase() == CEmbersLook(seedFor("DP-4")).eventBase());
    CHECK(CEmbersLook(seedFor("DP-4")).eventBase() != CEmbersLook(seedFor("DP-5")).eventBase());
}

// The heat moved once per frame by n steps: the compounded factors are the per-step loop.
// Decay: T = max(T (1 - c1) - c2, 0) n times is max(T rN - c2N, 0). Diffusion toward a
// fixed neighbour mean, T += 4 kappa (avg - T) n times, is mix(T, avg, aN). Sources, max()
// floors set at every step and then decayed, are max_k decayN(src_k, n - 1 - k).
TEST(embers_heat_factors_are_the_per_step_loop) {
    const double c1 = CEmbersLook::C1, c2 = CEmbersLook::C2, k4 = 4 * CEmbersLook::KAPPA;
    uint64_t     rng = 7;
    for (int n = 1; n <= 64; n++) {
        const auto f = CEmbersLook::heatFactors(n);
        for (const double T0 : {0.0, 0.001, 0.2, 0.9, 1.7}) {
            double T = T0;
            for (int i = 0; i < n; i++)
                T = std::max(T * (1 - c1) - c2, 0.0);
            CHECK_NEAR(std::max(T0 * f.rN - f.c2N, 0.0), T, 1e-12);
            CHECK_NEAR(CEmbersLook::decayN(T0, n), T, 1e-12);
            for (const double avg : {0.0, 0.5, 1.2}) {
                double D = T0;
                for (int i = 0; i < n; i++)
                    D += k4 * (avg - D);
                CHECK_NEAR(T0 + f.aN * (avg - T0), D, 1e-12);
            }
        }
        // decay and floors over a frame with random sources, no diffusion
        for (int trial = 0; trial < 20; trial++) {
            std::vector<double> src(static_cast<size_t>(n));
            for (auto& s : src)
                s = uniform01(rng) * 1.2;
            const double T0 = uniform01(rng) * 1.7;
            double       T  = T0;
            for (int k = 0; k < n; k++)
                T = std::max(std::max(T * (1 - c1) - c2, 0.0), src[static_cast<size_t>(k)]);
            double fl = 0;
            for (int k = 0; k < n; k++)
                fl = std::max(fl, CEmbersLook::decayN(src[static_cast<size_t>(k)], n - 1 - k));
            CHECK_NEAR(std::max(std::max(T0 * f.rN - f.c2N, 0.0), fl), T, 1e-12);
        }
    }
    CHECK(CEmbersLook::heatFactors(1).aN == k4);
}

// The lattice-cell rule on 400x225, 538x225 and 225x400: the stirrers' speeds and radius,
// the pops' radii, the wind and the gusts are the same in cells on every lattice; the
// pops' and the stirrers' places are proportional (the sketch's long axis along the long
// side); the heat bed's noise is a whole number of periods, about 40 cells each.
TEST(embers_lattice_cell_rule) {
    const CEmbersLook look(seedFor("bench"));
    const int         dims[3][2] = {{400, 225}, {538, 225}, {225, 400}};
    double            vmax[3]    = {0, 0, 0};
    for (int t = 0; t < 400000; t += 7)
        for (size_t i = 0; i < 3; i++)
            for (int l = 0; l < 3; l++) {
                const auto s = look.stirrer(i, t, dims[l][0], dims[l][1]);
                vmax[l]      = std::max(vmax[l], std::hypot(s.vx, s.vy));
            }
    std::printf("embers stirrers: max speed %.6f / %.6f / %.6f cells per step on 400x225 / 538x225 / 225x400\n", vmax[0], vmax[1], vmax[2]);
    CHECK(vmax[0] == vmax[1] && vmax[1] == vmax[2]);
    const auto a = look.stirrer(0, 1000, 400, 225), b = look.stirrer(0, 1000, 538, 225), c = look.stirrer(0, 1000, 225, 400);
    CHECK(std::abs((b.x - a.x) - 100.0 * (538.0 / 400.0 - 1.0)) < 1e-9 && b.y == a.y && b.vx == a.vx);
    CHECK(c.y == a.x && c.x == a.y && c.vy == a.vx && c.vx == a.vy);

    CEmbersEvents e[3] = {CEmbersEvents(99), CEmbersEvents(99), CEmbersEvents(99)};
    int           popsSeen = 0;
    for (uint64_t s = 0; s < 60000; s++) {
        CEmbersEvents::SStep o[3];
        for (int l = 0; l < 3; l++)
            o[l] = e[l].step(s, dims[l][0], dims[l][1]);
        // landscape: as computed; portrait: the sketch's x along y
        CHECK(o[1].wx == o[0].wx && o[1].wy == o[0].wy);
        CHECK(o[2].wy == o[0].wx && o[2].wx == o[0].wy);
        CHECK(o[0].pop == o[1].pop && o[1].pop == o[2].pop);
        if (o[0].pop) {
            popsSeen++;
            CHECK(o[1].popR == o[0].popR && o[2].popR == o[0].popR);
            CHECK_NEAR(o[1].popX, o[0].popX * 538.0 / 400.0, 1e-9);
            CHECK(o[1].popY == o[0].popY);
            CHECK_NEAR(o[2].popY, o[0].popX, 1e-9);
            CHECK(o[2].popX == o[0].popY);
        }
    }
    CHECK(popsSeen > 0);

    for (int l = 0; l < 3; l++) {
        const auto p        = CEmbersLook::bedPeriods(dims[l][0], dims[l][1]);
        const bool portrait = dims[l][0] < dims[l][1];
        const int  W = dims[l][0], H = dims[l][1];
        // cells per noise unit, per layer and axis: the sketch's 40 x 37.5 and 25 x 25
        const double c1x = static_cast<double>(W) / p.bed1[0], c1y = static_cast<double>(H) / p.bed1[1];
        const double c2x = static_cast<double>(W) / p.bed2[0], c2y = static_cast<double>(H) / p.bed2[1];
        const double fx = static_cast<double>(W) / p.flick[0], fy = static_cast<double>(H) / p.flick[1];
        std::printf("embers bed on %dx%d: periods (%d, %d) (%d, %d) flicker (%d, %d); cells per unit %.2f x %.2f, %.2f x %.2f, flicker %.2f x %.2f\n", W, H, p.bed1[0],
                    p.bed1[1], p.bed2[0], p.bed2[1], p.flick[0], p.flick[1], c1x, c1y, c2x, c2y, fx, fy);
        const double longC1 = portrait ? c1y : c1x, shortC1 = portrait ? c1x : c1y, longC2 = portrait ? c2y : c2x, shortC2 = portrait ? c2x : c2y;
        CHECK(std::abs(longC1 / 40.0 - 1) < 0.04 && std::abs(shortC1 / 37.5 - 1) < 1e-9);
        CHECK(std::abs(longC2 / 25.0 - 1) < 0.04 && std::abs(shortC2 / 25.0 - 1) < 1e-9);
        CHECK(std::abs(fx * 0.08 - 1) < 0.03 && std::abs(fy * 0.08 - 1) < 0.03);
    }
    const auto s = CEmbersLook::bedPeriods(400, 225);
    CHECK(s.bed1[0] == 10 && s.bed1[1] == 6 && s.bed2[0] == 16 && s.bed2[1] == 9 && s.flick[0] == 32 && s.flick[1] == 18);
}

// The noise offsets the shaders get stay below their periods however long the look runs,
// and are the sketch's own drift (mod the period) while small.
TEST(embers_noise_offsets_stay_small) {
    for (const int dims : {0, 1, 2}) {
        const int W = dims == 0 ? 400 : dims == 1 ? 538 : 225, H = dims == 2 ? 400 : 225;
        const auto p = CEmbersLook::bedPeriods(W, H);
        for (const double step : {0.0, 1.0, 4800.0, 1e6, 4.15e10}) {
            const auto o = CEmbersLook::noiseAt(step, W, H);
            for (int a = 0; a < 2; a++) {
                CHECK(o.bed1[a] >= 0 && o.bed1[a] < p.bed1[a]);
                CHECK(o.bed2[a] >= 0 && o.bed2[a] < p.bed2[a]);
            }
            CHECK(o.flick >= 0 && o.flick < (W < H ? p.flick[0] : p.flick[1]));
        }
    }
    // landscape, small t: the sketch's offsets (t shift 3e-5, t shift 1.1e-5), the second
    // layer's subtracted (t shift 2e-5, -t shift 2.6e-5), the flicker's t 4e-3
    const auto o = CEmbersLook::noiseAt(4800.0, 400, 225);
    CHECK_NEAR(o.bed1[0], std::fmod(4800.0 * 10 * 3e-5, 10.0), 1e-5);
    CHECK_NEAR(o.bed1[1], std::fmod(4800.0 * 10 * 1.1e-5, 6.0), 1e-5);
    CHECK_NEAR(o.bed2[0], std::fmod(4800.0 * 10 * 2e-5, 16.0), 1e-5);
    CHECK_NEAR(o.bed2[1], 9.0 - std::fmod(4800.0 * 10 * 2.6e-5, 9.0), 1e-5);
    CHECK_NEAR(o.flick, std::fmod(4800.0 * 4e-3, 18.0), 1e-5);
}
