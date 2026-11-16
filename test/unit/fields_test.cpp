#include "check.hpp"
#include "core/Compound.hpp"

#include <cmath>

TEST(compound) {
    CHECK_NEAR(compound(2.5e-4, 1), 2.5e-4, 1e-12);
    CHECK_NEAR(compound(2.5e-4, 12), 1.0 - std::pow(1.0 - 2.5e-4, 12), 1e-12);
    CHECK(compound(0.0, 12) == 0.0 && compound(1.0, 3) == 1.0);
}
