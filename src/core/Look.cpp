#include "core/Look.hpp"
#include "looks/Embers.hpp"
#include "looks/Jupiter.hpp"
#include "looks/Marble.hpp"
#include "looks/Pattern.hpp"

#include <algorithm>
#include <array>

namespace {

constexpr std::array<std::string_view, 4> NAMES = {"pattern", "embers", "marble", "jupiter"};

} // namespace

SGeometry geometryFor(int monW, int monH) {
    constexpr long SHORT = 225;
    const long     w = std::max(1, monW), h = std::max(1, monH);
    const long     shortSide = std::min(w, h), longSide = std::max(w, h);
    // long * 225 / short, a half rounding up, in integers: 3440x1440 is 537.5 -> 538
    const int cellsLong = static_cast<int>((2 * longSide * SHORT + shortSide) / (2 * shortSide));
    return w >= h ? SGeometry{monW, monH, cellsLong, static_cast<int>(SHORT)} : SGeometry{monW, monH, static_cast<int>(SHORT), cellsLong};
}

bool ILook::healthy() {
    checkHealth();
    return takeHealth(true) != EHealth::BLOWN;
}

std::span<const std::string_view> lookNames() {
    return NAMES;
}

bool isLookName(std::string_view name) {
    return std::ranges::find(NAMES, name) != NAMES.end();
}

std::string lookNameList() {
    std::string out;
    for (const auto name : NAMES)
        out += std::string{out.empty() ? "" : ", "} + std::string{name};
    return out;
}

std::unique_ptr<ILook> makeLook(std::string_view name, uint64_t seed) {
    if (!isLookName(name))
        return nullptr;
    if (name == "pattern")
        return std::make_unique<CPatternLook>(seed);
    if (name == "marble")
        return std::make_unique<CMarbleLook>(seed);
    if (name == "embers")
        return std::make_unique<CEmbersLook>(seed);
    if (name == "jupiter")
        return std::make_unique<CJupiterLook>(seed);
    return nullptr; // not reached: every name in NAMES has its look above
}

float clockRate(const ILook& look, float timeScale) {
    return look.stepsPerSecond() * std::max(timeScale, 0.0f);
}

uint64_t seedFor(std::string_view monitorName) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char c : monitorName) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}
