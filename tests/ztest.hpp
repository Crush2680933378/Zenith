// ============================================================================
//  ZTEST —— Zenith 自研微型测试框架（零三方依赖）
//  用法：
//    ZTEST(Core, ResultOk) { ZEXPECT_EQ(r.value(), 42); }
//    ZTEST_MAIN
// ============================================================================
#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace ztest {

struct TestCase {
    const char* suite;
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct FailException {};

[[noreturn]] inline void reportFail(const char* file, int line, const std::string& what) {
    std::fprintf(stderr, "      << %s:%d  %s\n", file, line, what.c_str());
    throw FailException{};
}

inline bool add(const char* suite, const char* name, void (*fn)()) {
    registry().push_back(TestCase{suite, name, fn});
    return true;
}

template <typename T>
std::string repr(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}
inline std::string repr(const std::string& v) { return '"' + v + '"'; }
inline std::string repr(bool v) { return v ? "true" : "false"; }
inline std::string repr(const char* v) { return v ? '"' + std::string(v) + '"' : std::string("(null)"); }

inline int runAll() {
    int total = 0, failed = 0;
    for (const TestCase& tc : registry()) {
        ++total;
        std::printf("[ RUN  ] %s.%s\n", tc.suite, tc.name);
        std::fflush(stdout);
        try {
            tc.fn();
            std::printf("[  OK  ] %s.%s\n", tc.suite, tc.name);
        } catch (const FailException&) {
            ++failed;
            std::printf("[ FAIL ] %s.%s\n", tc.suite, tc.name);
        } catch (...) {
            ++failed;
            std::printf("[ FAIL ] %s.%s (unexpected exception)\n", tc.suite, tc.name);
        }
    }
    std::printf("========== %d test(s), %d failed ==========\n", total, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace ztest

// ---------------------------------------------------------------------------
// 宏
// ---------------------------------------------------------------------------
#define ZTEST(suite_, name_)                                                         \
    static void ztest_fn_##suite_##_##name_();                                       \
    static bool ztest_reg_##suite_##_##name_ =                                       \
        ::ztest::add(#suite_, #name_, &ztest_fn_##suite_##_##name_);                 \
    static void ztest_fn_##suite_##_##name_()

#define ZEXPECT_TRUE(expr_)                                                           \
    do {                                                                              \
        if (!(expr_))                                                                \
            ::ztest::reportFail(__FILE__, __LINE__, "expected true: " #expr_);        \
    } while (0)

#define ZEXPECT_FALSE(expr_)                                                          \
    do {                                                                              \
        if (expr_)                                                                    \
            ::ztest::reportFail(__FILE__, __LINE__, "expected false: " #expr_);       \
    } while (0)

#define ZEXPECT_EQ(a_, b_)                                                            \
    do {                                                                              \
        auto&& za_ = (a_);                                                            \
        auto&& zb_ = (b_);                                                            \
        if (!(za_ == zb_))                                                            \
            ::ztest::reportFail(__FILE__, __LINE__,                                   \
                                "expected " #a_ " == " #b_ "; got " +                 \
                                    ::ztest::repr(za_) + " vs " +                     \
                                    ::ztest::repr(zb_));                              \
    } while (0)

#define ZEXPECT_NE(a_, b_)                                                            \
    do {                                                                              \
        auto&& za_ = (a_);                                                            \
        auto&& zb_ = (b_);                                                            \
        if (!(za_ != zb_))                                                            \
            ::ztest::reportFail(__FILE__, __LINE__,                                   \
                                "expected " #a_ " != " #b_ "; both " +                \
                                    ::ztest::repr(za_));                              \
    } while (0)

#define ZEXPECT_THROW(expr_, exType_)                                                 \
    do {                                                                              \
        bool zcaught_ = false;                                                        \
        try {                                                                         \
            (void)(expr_);                                                            \
        } catch (const exType_&) {                                                    \
            zcaught_ = true;                                                          \
        }                                                                             \
        if (!zcaught_)                                                                \
            ::ztest::reportFail(__FILE__, __LINE__,                                   \
                                "expected exception " #exType_ " from " #expr_);      \
    } while (0)

#define ZTEST_MAIN                                                                   \
    int main() { return ::ztest::runAll(); }
