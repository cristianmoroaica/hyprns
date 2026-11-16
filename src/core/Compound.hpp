#pragma once

// Pure C++, no GL: the unit tests link it without a context.

// The fraction a per-step rate (mix(v, target, perStep)) reaches over n steps:
// 1 - (1 - perStep)^n. For a field transported once over n fluid steps whose post
// does what n single steps would. 0 for n <= 0 or perStep <= 0, 1 for perStep >= 1.
double compound(double perStep, int n);

// A relaxation run n times on a constant input: d = (d + u) r per step, n times, is
// d r^n + u r (1 - r^n) / (1 - r) (the marble's displacement, moved once per frame).
// power = r^n, sum = r (1 - r^n) / (1 - r) (n when r = 1); {1, 0} for n <= 0.
struct SRelaxSum {
    double power, sum;
};
SRelaxSum relaxSum(double r, int n);
