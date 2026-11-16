#include "check.hpp"
#include "core/Look.hpp"

// The short side is 225 cells, the long side follows the aspect (half rounds up).
TEST(geometry) {
    CHECK(geometryFor(3440, 1440).cellsW == 538 && geometryFor(3440, 1440).cellsH == 225);
    CHECK(geometryFor(1440, 2560).cellsW == 225 && geometryFor(1440, 2560).cellsH == 400);
    CHECK(geometryFor(1920, 1080).cellsW == 400 && geometryFor(1920, 1080).cellsH == 225);
}
