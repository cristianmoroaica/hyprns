#include "check.hpp"

// The harness itself: a passing CHECK and CHECK_NEAR count as one passed test.
TEST(check_harness) {
    CHECK(1 + 1 == 2);
    CHECK_NEAR(0.1 + 0.2, 0.3, 1e-12);
}
