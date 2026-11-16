#include "core/Compound.hpp"

#include <cmath>

double compound(double perStep, int n) {
    if (n <= 0 || !(perStep > 0.0))
        return 0.0;
    if (perStep >= 1.0)
        return 1.0;
    // -expm1(n log1p(-p)) keeps the digits of a small rate that 1 - pow(1 - p, n) loses
    return -std::expm1(static_cast<double>(n) * std::log1p(-perStep));
}

SRelaxSum relaxSum(double r, int n) {
    if (n <= 0)
        return {1.0, 0.0};
    if (r == 1.0)
        return {1.0, static_cast<double>(n)};
    const double power = std::pow(r, n);
    return {power, r * (1.0 - power) / (1.0 - r)};
}
