#include "check.hpp"
#include "core/Sequence.hpp"

// SplitMix64 (Steele, Lea, Flood 2014): the published reference stream from state 0,
// and uniform01: the top 53 bits as a double in [0, 1).
TEST(splitmix_reference) {
    uint64_t s = 0;
    CHECK(splitmix(s) == 0xe220a8397b1dcdafull);
    CHECK(splitmix(s) == 0x6e789e6aa1b965f4ull);
    CHECK(splitmix(s) == 0x06c45d188009454full);
    CHECK(s == 3 * 0x9e3779b97f4a7c15ull);
}

TEST(uniform01_reference) {
    uint64_t s = 0;
    CHECK(uniform01(s) == 0.8833108082136426);
    CHECK(uniform01(s) == 0.43152799704850997);
    uint64_t t = 12345;
    CHECK(uniform01(t) == 0.1330796686614273);
}
