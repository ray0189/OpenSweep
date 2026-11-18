// Tiny self-contained test runner (no third-party dependencies).
// TEST(name) { CHECK(cond); CHECK_EQ(a, b); }  — failures are counted and reported; the
// process exits non-zero if any check fails, which CTest reports as a test failure.
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void report_failure(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, what.c_str());
}

template <typename T>
std::string to_str(const T& v) {
    if constexpr (requires(std::ostringstream& os, const T& x) { os << x; }) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else {
        return "<value>";
    }
}

}  // namespace testfw

#define TF_CONCAT2(a, b) a##b
#define TF_CONCAT(a, b) TF_CONCAT2(a, b)

#define TEST(name)                                                                    \
    static void name();                                                               \
    static const ::testfw::Registrar TF_CONCAT(tf_reg_, name)(#name, &name);          \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) ::testfw::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                    \
        const auto& tf_a = (a);                                                                             \
        const auto& tf_b = (b);                                                                             \
        if (!(tf_a == tf_b))                                                                                \
            ::testfw::report_failure(__FILE__, __LINE__,                                                    \
                                     "CHECK_EQ(" #a ", " #b "): " + ::testfw::to_str(tf_a) + " vs " +       \
                                         ::testfw::to_str(tf_b));                                           \
    } while (0)

#define CHECK_THROWS(expr)                                                                \
    do {                                                                                  \
        bool tf_threw = false;                                                            \
        try {                                                                             \
            (void)(expr);                                                                 \
        } catch (...) {                                                                   \
            tf_threw = true;                                                              \
        }                                                                                 \
        if (!tf_threw) ::testfw::report_failure(__FILE__, __LINE__, "CHECK_THROWS(" #expr ")"); \
    } while (0)
