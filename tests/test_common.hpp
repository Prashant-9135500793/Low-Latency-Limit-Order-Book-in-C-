#pragma once
// Minimal header-only test harness. No external dependency (e.g.
// GoogleTest) is pulled in, to keep the project self-contained and
// the build fast; the assertion surface needed here is small.

#include <cstdio>
#include <cstdlib>
#include <string>

inline int g_tests_run = 0;
inline int g_tests_failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_tests_run;                                                      \
        if (!(cond)) {                                                       \
            ++g_tests_failed;                                                \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__,      \
                          __LINE__);                                         \
        }                                                                    \
    } while (0)

#define RUN(test_fn)                                                         \
    do {                                                                     \
        std::printf("[ RUN  ] %s\n", #test_fn);                             \
        test_fn();                                                           \
        std::printf("[ DONE ] %s\n", #test_fn);                             \
    } while (0)

inline int test_summary() {
    std::printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
