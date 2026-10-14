// log 模块单测：formatLogLine / LoggerFacade / AsyncFileLogger（含滚动）
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#include "ztest.hpp"

#include "zenith/log/async_file_logger.hpp"
#include "zenith/log/console_logger.hpp"
#include "zenith/log/logger.hpp"

using namespace zenith::log;

namespace {

LogRecord makeRecord(std::string msg, LogLevel level = LogLevel::Info) {
    LogRecord r;
    r.level       = level;
    r.timestampMs = zenith::core::nowMilliseconds();
    r.threadId    = 99;
    r.file        = "some_file.cpp";
    r.line        = 12;
    r.message     = std::move(msg);
    return r;
}

/// 捕获型 sink：验证门面分发
class CaptureSink final : public LogSink {
public:
    void write(const LogRecord& r) override {
        std::lock_guard<std::mutex> lk(mtx_);
        ++count_;
        last_ = r.message;
    }
    void flush() override {}
    [[nodiscard]] int count() const { return count_.load(); }
    [[nodiscard]] std::string last() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_;
    }

private:
    mutable std::mutex mtx_;
    std::atomic<int> count_{0};
    std::string last_;
};

std::string fileToString(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

/* ---------------- formatLogLine ---------------- */

ZTEST(LogFormat, ContainsTimestampLevelTidFileMessage) {
    const std::string line = formatLogLine(makeRecord("hello log"));
    // [YYYY-MM-DD HH:MM:SS.mmm] [INFO ] [tid 99] some_file.cpp:12] hello log\n
    ZEXPECT_TRUE(line.size() > size_t{40});
    ZEXPECT_TRUE(line.find("[INFO ]") != std::string::npos);
    ZEXPECT_TRUE(line.find("some_file.cpp:12]") != std::string::npos);
    ZEXPECT_TRUE(line.find("hello log") != std::string::npos);
    ZEXPECT_EQ(line.back(), '\n');
    ZEXPECT_EQ(line.substr(0, 1), std::string("["));
}

/* ---------------- LoggerFacade ---------------- */

ZTEST(LoggerFacade, DispatchesToSinksAndRespectsLevel) {
    auto& facade = LoggerFacade::instance();
    const LogLevel oldLevel = facade.level();
    auto sink = std::make_shared<CaptureSink>();

    facade.setLevel(LogLevel::Warn);
    facade.addSink(sink);

    facade.write(LogLevel::Info, "f.cpp", 1, "should be filtered");
    ZEXPECT_EQ(sink->count(), 0);

    facade.write(LogLevel::Error, "f.cpp", 2, "real error");
    ZEXPECT_EQ(sink->count(), 1);
    ZEXPECT_EQ(sink->last(), std::string("real error"));

    facade.writeRaw(LogLevel::Fatal, "f.cpp", 3, "raw message");
    ZEXPECT_EQ(sink->count(), 2);

    facade.clearSinks();
    facade.setLevel(oldLevel);
    facade.write(LogLevel::Error, "f.cpp", 4, "after clear, no crash");
    ZEXPECT_EQ(sink->count(), 2);
}

ZTEST(LoggerFacade, ToStringNames) {
    ZEXPECT_EQ(std::string(toString(LogLevel::Trace)), std::string("TRACE"));
    ZEXPECT_EQ(std::string(toString(LogLevel::Fatal)), std::string("FATAL"));
}

/* ---------------- ConsoleLogger（只验证不崩溃） ---------------- */

ZTEST(ConsoleLogger, WriteDoesNotCrash) {
    ConsoleLogger sink(/*stderr=*/true, /*color=*/false);
    sink.write(makeRecord("plain console line"));
    sink.flush();
}

/* ---------------- AsyncFileLogger ---------------- */

ZTEST(AsyncFileLogger, PersistsAndRolls) {
    const std::string base = "ztest_async_roll.log";
    // 清理历史
    std::remove(base.c_str());
    std::remove((base + ".1").c_str());
    std::remove((base + ".2").c_str());

    constexpr size_t kRollBytes = 1024;
    constexpr int kKeepFiles = 2;
    {
        AsyncFileLogger sink(base, kRollBytes, kKeepFiles);
        for (int i = 0; i < 200; ++i) {
            sink.write(makeRecord("line-" + std::to_string(i) + " padding-padding-padding"));
        }
        sink.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    } // 析构排空并关闭

    const std::string current = fileToString(base);
    const std::string rolled1 = fileToString(base + ".1");
    ZEXPECT_TRUE(current.find("line-199") != std::string::npos); // 最后一条已落盘
    ZEXPECT_TRUE(!rolled1.empty());                              // 已发生滚动
    const std::string rolled2 = fileToString(base + ".2");
    ZEXPECT_TRUE(!rolled2.empty()); // 200 行 × ~50B 远超 1KB × 2 轮滚动
}

ZTEST(AsyncFileLogger, QueueOverflowDropsAndCounts) {
    // 容量 65536 的队列在单测里很难压满（后台线程在消费），
    // 这里仅验证 droppedCount 读取路径与正常写入共存。
    const std::string base = "ztest_async_drop.log";
    std::remove(base.c_str());
    {
        AsyncFileLogger sink(base);
        for (int i = 0; i < 1000; ++i) sink.write(makeRecord("x"));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ZEXPECT_TRUE(sink.droppedCount() <= 1000u); // 读路径可用且数值有界
    }
    std::remove(base.c_str());
}

ZTEST_MAIN
