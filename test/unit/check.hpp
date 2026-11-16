#pragma once

// A minimal host test harness, no GL.
//   TEST(name) { CHECK(cond); CHECK_NEAR(a, b, eps); }
// The Makefile compiles this header once more with HYPRNS_CHECK_MAIN defined as the
// program's main: it runs every TEST, prints "N passed, M failed" and exits non-zero
// on a failure. A test fails when any of its checks fails.

#include <cmath>
#include <cstdio>
#include <vector>

namespace Check {

struct STest {
    const char* name;
    void (*fn)();
};

inline std::vector<STest>& registry() {
    static std::vector<STest> tests;
    return tests;
}

inline int& failures() { // checks failed in the test that is running
    static int n = 0;
    return n;
}

inline bool registerTest(const char* name, void (*fn)()) {
    registry().push_back({name, fn});
    return true;
}

inline void fail(const char* file, int line, const char* what) {
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, what);
    failures()++;
}

} // namespace Check

#define CHECK(cond)                                                                                                                                  \
    do {                                                                                                                                             \
        if (!(cond))                                                                                                                                 \
            ::Check::fail(__FILE__, __LINE__, #cond);                                                                                                \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                                                        \
    do {                                                                                                                                             \
        const double check_a_ = static_cast<double>(a), check_b_ = static_cast<double>(b);                                                          \
        if (!(std::fabs(check_a_ - check_b_) <= static_cast<double>(eps))) {                                                                         \
            std::fprintf(stderr, "  %s = %.9g, %s = %.9g\n", #a, check_a_, #b, check_b_);                                                            \
            ::Check::fail(__FILE__, __LINE__, "CHECK_NEAR(" #a ", " #b ", " #eps ")");                                                               \
        }                                                                                                                                            \
    } while (0)

#define TEST(name)                                                                                                                                   \
    static void       check_test_##name();                                                                                                           \
    static const bool check_registered_##name = ::Check::registerTest(#name, &check_test_##name);                                                   \
    static void       check_test_##name()

#ifdef HYPRNS_CHECK_MAIN
int main() {
    int passed = 0, failed = 0;
    for (const auto& t : ::Check::registry()) {
        ::Check::failures() = 0;
        t.fn();
        if (::Check::failures() == 0)
            passed++;
        else {
            failed++;
            std::fprintf(stderr, "FAILED: %s\n", t.name);
        }
    }
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
#endif
