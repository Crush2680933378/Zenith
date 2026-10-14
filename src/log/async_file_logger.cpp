#include "zenith/log/async_file_logger.hpp"

#include <chrono>
#include <cerrno>
#include <cstring>

namespace zenith::log {

namespace {
// 后台线程每轮最多批量落盘的条数，避免长时间独占 pop 循环。
constexpr size_t kBatchSize = 256;
} // namespace

AsyncFileLogger::AsyncFileLogger(std::string basePath, size_t rollBytes, int keepFiles)
    : path_(std::move(basePath)), rollBytes_(rollBytes), keepFiles_(keepFiles > 0 ? keepFiles : 1) {
    openFile();
    worker_ = std::thread([this] { workerLoop(); });
}

AsyncFileLogger::~AsyncFileLogger() {
    stop_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(wakeMtx_);
        wakeCv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    drainBatch(); // 析构前最后排空一次
    closeFile();
}

void AsyncFileLogger::write(const LogRecord& record) {
    if (stop_.load(std::memory_order_acquire)) return; // 已进入析构，直接丢弃
    if (!queue_->push(formatLogLine(record))) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // 仅在队列从空变非空时唤醒，降低后台线程无效唤醒
    std::lock_guard<std::mutex> lk(wakeMtx_);
    wakeCv_.notify_one();
}

void AsyncFileLogger::flush() {
    std::lock_guard<std::mutex> lk(wakeMtx_);
    wakeCv_.notify_all();
}

void AsyncFileLogger::workerLoop() {
    while (!stop_.load(std::memory_order_acquire)) {
        drainBatch();
        std::unique_lock<std::mutex> lk(wakeMtx_);
        wakeCv_.wait_for(lk, std::chrono::milliseconds(200), [this] {
            return stop_.load(std::memory_order_acquire) || !queue_->emptyApprox();
        });
    }
    drainBatch(); // 退出前排空
}

void AsyncFileLogger::drainBatch() {
    std::string line;
    for (size_t i = 0; i < kBatchSize && queue_->pop(line); ++i) {
        if (fp_ == nullptr && !openFile()) continue; // 打开失败则丢弃该行
        const size_t n = std::fwrite(line.data(), 1, line.size(), fp_);
        written_ += n;
        if (written_ >= rollBytes_) {
            std::fflush(fp_);
            rollFiles();
        }
    }
    if (fp_ != nullptr) std::fflush(fp_);
}

bool AsyncFileLogger::openFile() {
    fp_ = std::fopen(path_.c_str(), "ab");
    if (fp_ == nullptr) return false;
    // 以追加模式打开时无法得知当前大小，用 ftell 对二进制流近似定位
    const long pos = std::ftell(fp_);
    written_ = pos > 0 ? static_cast<size_t>(pos) : 0;
    if (written_ >= rollBytes_) rollFiles();
    return true;
}

void AsyncFileLogger::closeFile() {
    if (fp_ != nullptr) {
        std::fflush(fp_);
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

void AsyncFileLogger::rollFiles() {
    closeFile();
    // base.log.N-1 ← ... ← base.log.1 ← base.log
    for (int i = keepFiles_ - 1; i >= 1; --i) {
        const std::string from = path_ + "." + std::to_string(i);
        const std::string to   = path_ + "." + std::to_string(i + 1);
        std::remove(to.c_str());
        std::rename(from.c_str(), to.c_str());
    }
    if (keepFiles_ >= 1) {
        const std::string to = path_ + ".1";
        std::remove(to.c_str());
        std::rename(path_.c_str(), to.c_str());
    }
    written_ = 0;
    openFile();
}

} // namespace zenith::log
