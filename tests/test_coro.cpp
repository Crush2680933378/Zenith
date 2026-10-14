// coro 层冒烟测试（仅 ZENITH_ENABLE_COROUTINES=ON 时编译，C++20）
#include <stdexcept>
#include <string>

#include "ztest.hpp"

#include "zenith/coro/task.hpp"

using namespace zenith::coro;

ZTEST(Task, SimpleReturnValue) {
    auto make = []() -> Task<int> { co_return 42; };
    ZEXPECT_EQ(syncWait(make()), 42);
}

ZTEST(Task, NestedAwait) {
    auto inner = []() -> Task<std::string> { co_return "inner-value"; };
    auto outer = [&]() -> Task<std::string> {
        const std::string v = co_await inner();
        co_return "outer(" + v + ")";
    };
    ZEXPECT_EQ(syncWait(outer()), std::string("outer(inner-value)"));
}

ZTEST(Task, VoidTask) {
    auto t = []() -> Task<void> { co_return; };
    syncWait(t()); // 编译 + 运行即通过（异常路径由下例覆盖）
    ZEXPECT_TRUE(true);
}

ZTEST(Task, MultipleAwaitsChain) {
    auto step = [](int x) -> Task<int> { co_return x + 1; };
    auto all = [&]() -> Task<int> {
        int a = co_await step(0); // 1
        int b = co_await step(a); // 2
        int c = co_await step(b); // 3
        co_return a + b + c;
    };
    ZEXPECT_EQ(syncWait(all()), 6);
}

ZTEST(Task, ExceptionPropagates) {
    auto boom = []() -> Task<int> { throw std::runtime_error("kaboom"); };
    ZEXPECT_THROW(syncWait(boom()), std::runtime_error);
}

ZTEST(Task, ExceptionThroughNestedAwait) {
    auto inner = []() -> Task<int> { throw std::logic_error("inner fault"); };
    auto outer = [&]() -> Task<int> { co_return co_await inner(); };
    ZEXPECT_THROW(syncWait(outer()), std::logic_error);
}

ZTEST_MAIN
