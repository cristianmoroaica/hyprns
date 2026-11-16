#pragma once

// Low-discrepancy sequences and easing shared by the looks.

#include <algorithm>
#include <cmath>
#include <cstdint>

constexpr double PHI = 0.6180339887, PLASTIC = 0.7548776662; // golden ratio - 1, plastic number reciprocal

// x - floor(x): in [0, 1) for negative x too
inline double frac(double x) {
    return x - std::floor(x);
}

// GLSL smoothstep: 0 below a, 1 above b, a cubic between; a == b gives a step at a
inline double smoothstep(double a, double b, double x) {
    if (a == b)
        return x < a ? 0.0 : 1.0;
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// SplitMix64: the next 64 random bits of the stream whose state is s (any start, 0 too)
inline uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z          = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z          = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// the next splitmix as a double in [0, 1), its top 53 bits
inline double uniform01(uint64_t& s) {
    return static_cast<double>(splitmix(s) >> 11) * 0x1.0p-53;
}
