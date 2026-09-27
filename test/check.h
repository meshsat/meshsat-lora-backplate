// SPDX-License-Identifier: GPL-3.0-or-later
// The smallest test harness that still says which line failed.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check
{
struct Case {
    std::string name;
    std::function<void()> run;
};
inline std::vector<Case> &cases()
{
    static std::vector<Case> all;
    return all;
}
inline int &failures()
{
    static int n = 0;
    return n;
}
struct Register {
    Register(const char *name, std::function<void()> fn) { cases().push_back({name, fn}); }
};
inline int runAll(int argc, char **argv)
{
    int failedCases = 0;
    const std::string only = argc > 1 ? argv[1] : "";
    for (auto &c : cases()) {
        if (!only.empty() && c.name.find(only) == std::string::npos)
            continue;
        const int before = failures();
        c.run();
        const bool ok = failures() == before;
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", c.name.c_str());
        failedCases += ok ? 0 : 1;
    }
    std::printf("%zu cases, %d failed\n", cases().size(), failedCases);
    return failedCases ? 1 : 0;
}
} // namespace check

#define TEST(name)                                                                                                     \
    static void test_##name();                                                                                         \
    static check::Register register_##name(#name, test_##name);                                                        \
    static void test_##name()

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("      %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);                                        \
            check::failures()++;                                                                                       \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        const long long va = (long long)(a), vb = (long long)(b);                                                      \
        if (va != vb) {                                                                                                \
            std::printf("      %s:%d  %s = %lld, expected %s = %lld\n", __FILE__, __LINE__, #a, va, #b, vb);           \
            check::failures()++;                                                                                       \
        }                                                                                                              \
    } while (0)
