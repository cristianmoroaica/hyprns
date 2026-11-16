#include "check.hpp"
#include "core/Bake.hpp"
#include "core/Compound.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

// Every row of the texture is in exactly one strip of a key, the strips are in order
// and differ in height by at most one row, for heights below, at and above the count.
TEST(bake_strips_cover_every_row_once) {
    for (const int rows : {1, 7, 29, 30, 31, 900, 1440, 1645, 2662})
        for (const int frames : {1, 2, 30, 60, 144}) {
            std::vector<int> hits(static_cast<size_t>(rows), 0);
            int              lo = rows, hi = 0, last = 0;
            for (int k = 0; k < frames; k++) {
                const auto s = CBakeSchedule::strip(k, rows, frames);
                CHECK(s.y0 == last && s.y1 >= s.y0 && s.y1 <= rows);
                last = s.y1;
                lo   = std::min(lo, s.y1 - s.y0);
                hi   = std::max(hi, s.y1 - s.y0);
                for (int y = s.y0; y < s.y1; y++)
                    hits[static_cast<size_t>(y)]++;
            }
            CHECK(last == rows && hi - lo <= 1);
            for (const int h : hits)
                CHECK(h == 1);
        }
}

// Over the frames: starting() on the first frame of each key, blend() k / F after k
// frames of a key, a rotation exactly every F frames, and the strips of one key baked in
// order 0..F-1, so a key is complete when it rotates.
TEST(bake_schedule_over_frames) {
    const int     rows = 1645, frames = 30;
    CBakeSchedule s;
    s.reset(rows, frames);
    CHECK(s.starting() && s.blend() == 0.0f);
    int covered = 0, rotations = 0;
    for (int f = 0; f < 95; f++) {
        const bool startingBefore = s.starting();
        CHECK(startingBefore == (f % frames == 0));
        CBakeSchedule::SStrip strip{};
        const bool            rotated = s.advance(&strip);
        const auto            want    = CBakeSchedule::strip(f % frames, rows, frames);
        CHECK(strip.y0 == want.y0 && strip.y1 == want.y1);
        covered += strip.y1 - strip.y0;
        CHECK(rotated == ((f + 1) % frames == 0));
        if (rotated) {
            CHECK(covered == rows); // the key that rotates in has every row
            covered = 0;
            rotations++;
        }
        CHECK_NEAR(s.blend(), static_cast<double>((f + 1) % frames) / frames, 1e-7);
    }
    CHECK(rotations == 3);
}

// The marble's displacement over n fluid steps in one go: d r^n + u r (1 - r^n)/(1 - r)
// is the per-step loop d = (d + u) r run n times (the approved sketch's post), for a
// constant u; not (d + u n) r^n.
TEST(relax_sum_matches_the_per_step_loop) {
    const double r = 0.975;
    for (int n = 0; n <= 12; n++) {
        const auto rs = relaxSum(r, n);
        for (const double d0 : {-7.5, 0.0, 3.25})
            for (const double u : {-0.31, 0.0, 0.07}) {
                double d = d0;
                for (int i = 0; i < n; i++)
                    d = (d + u) * r;
                CHECK_NEAR(d0 * rs.power + u * rs.sum, d, 1e-12);
            }
    }
    CHECK(relaxSum(r, 0).power == 1.0 && relaxSum(r, 0).sum == 0.0);
    CHECK_NEAR(relaxSum(r, 1).power, r, 1e-15);
    CHECK_NEAR(relaxSum(r, 1).sum, r, 1e-15);
    // r = 1: no relaxation, the sum is n
    CHECK_NEAR(relaxSum(1.0, 5).sum, 5.0, 1e-15);
    // the rejected form differs at n = 4 by u (n r^n - r (1 - r^n)/(1 - r))
    const auto rs4 = relaxSum(r, 4);
    std::printf("relax sum n=4: power %.12f, sum %.12f (the (d + u n) r^n form would use %.12f)\n", rs4.power, rs4.sum, 4.0 * rs4.power);
}

// The key clock over a bake's life, as a look drives it (CMarbleLook::bakeAdvance):
// priming at the frame's step, the first back key one nominal period after next, then
// each back key the steps of the last key period after next; rotations shift the times.
TEST(key_clock_latches_and_rotates) {
    const int     frames  = 30;
    const double  nominal = 120.0; // 120 steps/s, a key per second
    CBakeSchedule s;
    CKeyClock     k;
    k.requestPrime();
    // frame 1 primes (the bake is not ready during it): 4 steps ran before
    double step = 4;
    k.frame(step, nominal, false, false, 4);
    CHECK(k.keyTime(0) == 4 && k.keyTime(1) == 124);
    CHECK(k.shown(false, 0.0f) == 4);
    s.reset(1000, frames);
    // steady frames: 3 steps each (a slower clock than nominal)
    for (int f = 0; f < 2 * frames; f++) {
        const bool starting = s.starting();
        step += 3;
        k.frame(step, nominal, true, starting, 3);
        if (f == 0)
            CHECK(k.keyTime(2) == 124 + nominal); // the first back key: nominal after next
        if (f == frames)
            CHECK(k.keyTime(2) == 244 + 3 * frames); // then the steps of the last period
        if (s.advance(nullptr))
            k.rotated();
        if (f == frames - 1)
            CHECK(k.keyTime(0) == 124 && k.keyTime(1) == 244);
    }
    CHECK(k.keyTime(0) == 244 && k.keyTime(1) == 334);
    CHECK_NEAR(k.shown(true, 0.5f), 289.0, 1e-9);
}

// A rebuild primes from the time shown at that moment; a second one while the first
// primes keeps it (the held picture shows that time), and a pending request is kept.
TEST(key_clock_prime_from_shown) {
    CKeyClock k;
    k.requestPrime();
    k.frame(10, 100, false, false, 2);
    k.frame(12, 100, true, true, 2); // back latched at next + nominal
    CHECK(k.keyTime(0) == 10 && k.keyTime(1) == 110 && k.keyTime(2) == 210);
    k.requestPrimeFromShown(true, 0.25f); // shown: 10 + 0.25 * 100
    k.frame(500, 100, false, false, 2);
    CHECK(k.keyTime(0) == 35 && k.keyTime(1) == 135);
    k.requestPrimeFromShown(false, 0.9f); // the held picture shows the priming pair's start
    k.frame(510, 100, false, false, 2);
    CHECK(k.keyTime(0) == 35 && k.keyTime(1) == 135);
    k.requestPrime();                     // a reset: the frame's own step
    k.requestPrimeFromShown(true, 0.5f); // pending: kept as is
    k.frame(0, 100, false, false, 0);
    CHECK(k.keyTime(0) == 0 && k.keyTime(1) == 100);
}
