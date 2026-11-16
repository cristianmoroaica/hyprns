#include "check.hpp"
#include "core/Compound.hpp"
#include "core/Look.hpp"
#include "core/Sequence.hpp"
#include "looks/Jupiter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// The brief's test, verbatim.
TEST(jupiter_schedule) {
    CJupiterSchedule s(400, 225, 0);
    CHECK(s.jets() == 7);
    CHECK(CJupiterSchedule(225, 400, 0).jets() == 13);
    CHECK(CJupiterSchedule(538, 225, 0).jets() == 7);
    s.advanceTo(499);   CHECK(s.storms().empty());
    s.advanceTo(700);   CHECK(!s.storms().empty() && s.storms().size() <= 4);
    for (const auto& q : s.storms()) CHECK(q.r >= 11 && q.r <= 23);
    s.advanceTo(2400);  CHECK(s.eye().envelope == 0.0);
    s.advanceTo(4100);  CHECK_NEAR(s.eye().envelope, 1.0, 1e-9);      // ramp in over 1500 from 2500
    CHECK_NEAR(s.eye().r, 20.0, 1e-9);
    double y = s.eye().y / 225.0 * 7.0; CHECK_NEAR(y, std::round(y), 1e-9); CHECK(int(std::round(y)) % 2 == 1); // an odd band edge
    s.advanceTo(2500 + 14000 + 1); CHECK(s.eye().envelope == 0.0);
}

namespace {

double ss(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// sketchJove's frame loop (reference/b7-sketch.html), events only, on a W x H lattice with
// n jets in place of 7 and the event counters starting at `base` / `baseEye` (the sketch's
// at 0): what it hands the force at each step (st[16] and eye[4]).
struct SSketchJove {
    double W, H;
    int    n;
    double base, baseEye;
    double ev = 0, next = 500, en = 0, e0 = 2500, ex = 0, ey = 0;
    struct SQ {
        double x, y, r, s, c, t0, dur;
    };
    std::vector<SQ> storms{};
    double          eye[4] = {0, 0, 0, 0}, st[16] = {};
    void            step(double step) {
        const double ELIFE = 14000;
        if (step >= next) {
            ev++;
            const double i = base + ev;
            next           = step + 6500 + 5000 * frac(i * PHI);
            const double edge = ((std::floor(frac(i * PLASTIC) * (n - 1)) + 1) / n) * H, x0 = frac(i * PHI) * W;
            const int    cnt  = 3 + static_cast<int>(std::fmod(i, 2.0));
            storms.clear();
            for (int k = 0; k < cnt; k++) {
                const double h = frac((i * 7 + k * 3) * 0.5698402910);
                storms.push_back({std::fmod(x0 + k * (55 + 30 * h), W), edge + (h - 0.5) * 34, 11 + 12 * h, (k % 2 ? -1.0 : 1.0) * (0.7 + 0.6 * h),
                                  frac(k * 0.37 + i * 0.21) > 0.5 ? 0.5 : (k % 2 ? 0.0 : 0.72), step + k * 260, 1500 + 900 * h});
            }
        }
        if (step == e0) {
            en++;
            const double j = baseEye + en;
            const int    m = (n - 1) / 2;
            // the sketch's [1, 5, 3][en % 3]: the odd edges walked downwards from 1
            const int jm = static_cast<int>(std::fmod(j, static_cast<double>(m)));
            ey           = (2 * ((m - jm) % m) + 1) / static_cast<double>(n) * H;
            ex           = frac(j * PLASTIC + 0.3) * W;
        }
        const double dt = step - e0;
        if (dt >= 0 && dt <= ELIFE) {
            eye[0] = std::fmod(ex - 0.004 * dt + 4 * W, W);
            eye[1] = ey;
            eye[2] = 20;
            eye[3] = ss(0, 1500, dt) * ss(ELIFE, ELIFE - 2500, dt);
        } else {
            eye[3] = 0;
            if (dt > ELIFE)
                e0 = step + 9000 + std::floor(8000 * frac((baseEye + en) * PHI));
        }
        std::fill(std::begin(st), std::end(st), 0.0);
        for (size_t i = 0; i < storms.size(); i++) {
            const auto&  q  = storms[i];
            const double dq = step - q.t0;
            if (dq < 0 || dq > q.dur)
                continue;
            const double e = ss(0, 250, dq) * ss(q.dur, q.dur - 500, dq);
            st[i * 4] = q.x, st[i * 4 + 1] = q.y, st[i * 4 + 2] = q.r, st[i * 4 + 3] = q.s * e;
        }
    }
};

} // namespace

// The schedule is the sketch's frame loop step for step, the seed only moving where its two
// counters start; on the 400x225 lattice with the counters at 0 it is the sketch's own.
TEST(jupiter_schedule_is_the_sketch_from_the_seed) {
    const int dims[3][2] = {{400, 225}, {538, 225}, {225, 400}};
    for (const uint64_t seed : {uint64_t{0}, uint64_t{1}, seedFor("DP-4"), seedFor("DP-5")})
        for (const auto& d : dims) {
            CJupiterSchedule sc(d[0], d[1], seed);
            SSketchJove      sk{static_cast<double>(d[0]), static_cast<double>(d[1]), sc.jets(), static_cast<double>(sc.stormBase()), static_cast<double>(sc.eyeBase())};
            int              alive = 0, eyeUp = 0;
            for (uint64_t t = 0; t < 60000; t++) {
                sc.advanceTo(t);
                sk.step(static_cast<double>(t));
                // the active storms, in the sketch's slots
                size_t a = 0;
                for (int i = 0; i < 4; i++) {
                    if (sk.st[i * 4 + 2] <= 0)
                        continue;
                    if (a >= sc.storms().size()) {
                        CHECK(false);
                        break;
                    }
                    const auto& q = sc.storms()[a++];
                    CHECK_NEAR(q.x, sk.st[i * 4], 1e-9);
                    CHECK_NEAR(q.y, sk.st[i * 4 + 1], 1e-9);
                    CHECK_NEAR(q.r, sk.st[i * 4 + 2], 1e-12);
                    CHECK_NEAR(q.strength, sk.st[i * 4 + 3], 1e-12);
                    CHECK(q.slot == i);
                }
                CHECK(a == sc.storms().size());
                alive += a > 0;
                CHECK_NEAR(sc.eye().envelope, sk.eye[3], 1e-12);
                if (sk.eye[3] > 0) {
                    eyeUp++;
                    CHECK_NEAR(sc.eye().x, sk.eye[0], 1e-9);
                    CHECK_NEAR(sc.eye().y, sk.eye[1], 1e-9);
                }
            }
            CHECK(alive > 5000 && eyeUp > 10000);
        }
    // seed 0 is the sketch's own sequence: the first outbreak's edge and x as sketchJove's ev = 1
    CJupiterSchedule s0(400, 225, 0);
    CHECK(s0.stormBase() == 0 && s0.eyeBase() == 0);
    s0.advanceTo(500);
    CHECK(s0.storms().size() == 1 + 0); // only the first of 4 (ev 1: 3 + 1) has started
    CHECK_NEAR(s0.storms()[0].x, frac(PHI) * 400, 1e-9);
    s0.advanceTo(2500);
    CHECK_NEAR(s0.eye().y, 5.0 / 7.0 * 225.0, 1e-9); // the sketch's [1, 5, 3][1]
    CHECK(s0.outbreaks() == 1 && s0.eyes() == 1);
}

// Two monitors (two seeds) do not show the same storms at the same time.
TEST(jupiter_schedule_differs_per_monitor) {
    CJupiterSchedule a(538, 225, seedFor("DP-4")), b(538, 225, seedFor("DP-5"));
    CHECK(a.stormBase() != b.stormBase());
    int same = 0, both = 0;
    for (uint64_t t = 0; t < 40000; t += 50) {
        a.advanceTo(t);
        b.advanceTo(t);
        if (!a.storms().empty() && !b.storms().empty()) {
            both++;
            if (std::abs(a.storms()[0].x - b.storms()[0].x) < 1 && std::abs(a.storms()[0].y - b.storms()[0].y) < 1)
                same++;
        }
    }
    CHECK(both > 0 && same == 0);
}

// After a reset the look reseeds the schedule: another seed gives another first outbreak,
// the same seed the same one, and reseeding is a construction with that seed.
TEST(jupiter_schedule_reseed) {
    auto firstStorm = [](CJupiterSchedule& s) {
        s.advanceTo(600);
        return s.storms().empty() ? -1.0 : s.storms()[0].x * 1000 + s.storms()[0].y;
    };
    const uint64_t   seed = seedFor("bench");
    CJupiterSchedule a(225, 400, seed), b(225, 400, seed), c(225, 400, seed + 1);
    const double     fa = firstStorm(a);
    CHECK(fa >= 0);
    b.reseed(seed ^ 1);
    const double fb = firstStorm(b);
    CHECK(fb >= 0 && fb != fa);
    b.reseed(seed ^ 1);
    CHECK(firstStorm(b) == fb);
    b.reseed(seed);
    CHECK(firstStorm(b) == fa);
    c.reseed(seed ^ 1);
    CHECK(firstStorm(c) == fb && c.outbreaks() == 1);
    a.restart(); // restart alone replays
    CHECK(firstStorm(a) == fa);
}

// The cloud moved once per frame by n steps: every per-step mix factor a toward a target
// held for the frame becomes 1 - (1 - a)^n (the shader's comp, compound in C++), which is
// what n per-step mixes give; the relaxation's backN as well.
TEST(jupiter_cloud_factors_are_the_per_step_loop) {
    uint64_t rng = 11;
    for (int n = 1; n <= CJupiterLook::MAX_TRANSPORT_STEPS; n++) {
        CHECK_NEAR(CJupiterLook::backN(n), compound(CJupiterLook::BACK, n), 1e-15);
        for (int trial = 0; trial < 50; trial++) {
            // a storm or eye factor at some pixel (up to 0.004 x 1.3 and 0.008), a relaxation
            const double a      = trial == 0 ? CJupiterLook::BACK : uniform01(rng) * (trial % 2 ? 0.0052 : 0.008);
            const double target = uniform01(rng), v0 = uniform01(rng);
            double       v      = v0;
            for (int i = 0; i < n; i++)
                v = v + (target - v) * a; // mix(v, target, a)
            const double f = CJupiterLook::stepFactor(a, n);
            CHECK_NEAR(v0 + (target - v0) * f, v, 1e-12);
            CHECK_NEAR(f, compound(a, n), 1e-12);
        }
        // several mixes in a step (relaxation, then storms, then the eye, each toward its
        // own target): per frame they are applied one after another with their compounded
        // factors; the order inside a step changes the result by O(a b n^2) only
        const double ab = 2.5e-4, as = 0.004, ae = 0.008, tb = 0.3, ts = 0.72, te = 0.9;
        double       v  = 0.5;
        for (int i = 0; i < n; i++) {
            v += (tb - v) * ab;
            v += (ts - v) * as;
            v += (te - v) * ae;
        }
        double w = 0.5;
        w += (tb - w) * CJupiterLook::stepFactor(ab, n);
        w += (ts - w) * CJupiterLook::stepFactor(as, n);
        w += (te - w) * CJupiterLook::stepFactor(ae, n);
        CHECK(std::abs(v - w) <= 2.0 * (ab * as + ab * ae + as * ae) * n * n + 1e-12);
    }
    CHECK(CJupiterLook::stepFactor(0.004, 1) == 0.004);
    CHECK(CJupiterLook::stepFactor(0.0, 12) == 0.0);
}

// The lattice-cell rule on 400x225, 538x225 and 225x400: storm radii, strengths, spacing,
// offsets from their edge and lives, the eye's radius and drift are the same in cells on
// every lattice; only the outbreak's and the eye's x are placed proportionally, and the
// band edges at k H / n with n jets keeping their height near 32 cells.
TEST(jupiter_lattice_cell_rule) {
    const int dims[3][2] = {{400, 225}, {538, 225}, {225, 400}};
    std::vector<CJupiterSchedule> s;
    for (const auto& d : dims)
        s.emplace_back(d[0], d[1], seedFor("bench"));
    for (int l = 0; l < 3; l++) {
        const double band = static_cast<double>(dims[l][1]) / s[l].jets();
        std::printf("jupiter on %dx%d: %d jets, bands %.2f cells high\n", dims[l][0], dims[l][1], s[l].jets(), band);
        CHECK(s[l].jets() % 2 == 1 && std::abs(band / (225.0 / 7.0) - 1) < 0.05);
    }
    int    seen = 0;
    double eyeX0[3] = {-1, -1, -1};
    for (uint64_t t = 0; t < 60000; t++) {
        for (auto& q : s)
            q.advanceTo(t);
        const auto& a = s[0].storms();
        for (int l = 1; l < 3; l++) {
            const auto& b = s[l].storms();
            CHECK(a.size() == b.size());
            if (a.size() != b.size())
                continue;
            for (size_t i = 0; i < a.size(); i++) {
                CHECK(a[i].r == b[i].r && a[i].strength == b[i].strength && a[i].colour == b[i].colour && a[i].t0 == b[i].t0 && a[i].dur == b[i].dur);
                // the offset from the outbreak's edge in cells, the edge itself at k H / n
                const double ka = a[i].edge / 225.0 * 7.0, kb = b[i].edge / dims[l][1] * s[l].jets();
                CHECK_NEAR(ka, std::round(ka), 1e-9);
                CHECK_NEAR(kb, std::round(kb), 1e-9);
                CHECK_NEAR(a[i].y - a[i].edge, b[i].y - b[i].edge, 1e-9);
                // the spacing along x in cells: on 400 cells the offset (at most 3 x 85)
                // never wraps, so it is the true one; on the other lattice the storm sits
                // that many cells from the first, wrapped
                if (i > 0 && a[0].slot == 0 && b[0].slot == 0) {
                    const double Wb = dims[l][0];
                    const double da = std::fmod(a[i].x - a[0].x + 400.0, 400.0);
                    const double e  = std::fmod(b[0].x + da - b[i].x + 4 * Wb, Wb);
                    CHECK(std::min(e, Wb - e) < 1e-9);
                }
                // the outbreak's x proportional to the width
                if (i == 0 && b[i].t0 == t)
                    CHECK_NEAR(a[0].x / 400.0, b[0].x / dims[l][0], 1e-12);
                seen++;
            }
        }
        for (int l = 0; l < 3; l++) {
            const auto e = s[l].eye();
            if (e.envelope > 0) {
                CHECK(e.r == 20.0);
                const double y = e.y / dims[l][1] * s[l].jets();
                CHECK_NEAR(y, std::round(y), 1e-9);
                CHECK(static_cast<int>(std::round(y)) % 2 == 1);
                if (eyeX0[l] < 0)
                    eyeX0[l] = e.x;
            }
        }
    }
    CHECK(seen > 1000);
    // the first eye's start x is proportional (same fraction of each width); it is first
    // seen one step in, after a drift of 0.004 cells (the same in cells on every lattice)
    CHECK_NEAR((eyeX0[0] + 0.004) / 400.0, (eyeX0[1] + 0.004) / 538.0, 1e-9);
    CHECK_NEAR((eyeX0[0] + 0.004) / 400.0, (eyeX0[2] + 0.004) / 225.0, 1e-9);
}

// The grain renews 24 times a second of wall time, from an integer tick: no float that grows
// with run time, the same at any frame count, and fine after years of uptime.
TEST(jupiter_grain_tick) {
    CHECK(CJupiterLook::grainTick(0) == 0);
    CHECK(CJupiterLook::grainTick(41'666'666) == 0);
    CHECK(CJupiterLook::grainTick(41'666'667) == 1);
    CHECK(CJupiterLook::grainTick(1'000'000'000) == 24);
    const uint64_t years = 10ull * 365 * 24 * 3600 * 1'000'000'000ull; // ns
    CHECK(CJupiterLook::grainTick(years) == 10ull * 365 * 24 * 3600 * 24);
    CHECK(CJupiterLook::grainTick(~uint64_t{0}) == static_cast<uint64_t>((static_cast<unsigned __int128>(~uint64_t{0}) * 24) / 1'000'000'000u));
    int changes = 0;
    for (uint64_t k = 0; k < 2000; k++) {
        const uint32_t a = CJupiterLook::grainFrame(k), b = CJupiterLook::grainFrame(k + 1);
        CHECK(a < 4096 && b < 4096);
        changes += a != b;
        CHECK(CJupiterLook::grainFrame(years / 41'666'667 + k) < 4096);
    }
    CHECK(changes > 1990);
}
