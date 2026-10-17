#include "zenith/config/hot_reload.hpp"

#include <filesystem>
#include <fstream>

namespace zenith::config {

namespace fs = std::filesystem;

HotReloader::HotReloader(std::string path, unsigned intervalMs)
    : path_(std::move(path)), intervalMs_(intervalMs) {}

HotReloader::~HotReloader() { stop(); }

void HotReloader::start() {
    if (worker_.joinable()) return; // 幂等
    stop_.store(false, std::memory_order_release);

    reload(); // 首次装载（失败不阻塞启动——后续变更可恢复）

    if (intervalMs_ == 0) return; // 仅手动模式
    worker_ = std::thread([this] { monitorLoop(); });
}

void HotReloader::stop() {
    stop_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(wakeMtx_);
        wakeCv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

bool HotReloader::reload() {
    std::lock_guard<std::mutex> lk(wakeMtx_);
    return reloadLocked();
}

bool HotReloader::reloadLocked() {
    auto cfg = JsonConfig::loadFile(path_);
    if (!cfg) {
        if (onError_) onError_(cfg.error().message);
        return false;
    }
    auto fresh = std::make_shared<JsonConfig>(std::move(cfg).value());
    std::atomic_store(&current_,
                      std::shared_ptr<const JsonConfig>(std::move(fresh)));
    lastMtime_ = fileMtime();
    // 基线记录（用于轮询检测）
    if (auto content = readFile()) lastHash_ = fnv1a(*content);
    hasBaseline_ = true;
    loaded_.store(true, std::memory_order_release);
    reloadCount_.fetch_add(1, std::memory_order_relaxed);
    if (onChange_) onChange_(current_);
    return true;
}

std::optional<std::string> HotReloader::readFile() const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

uint64_t HotReloader::fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

int64_t HotReloader::fileMtime() const {
    std::error_code ec;
    const auto t = fs::last_write_time(path_, ec);
    if (ec) return 0;
    return static_cast<int64_t>(t.time_since_epoch().count());
}

void HotReloader::monitorLoop() {
    while (!stop_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(wakeMtx_);
        wakeCv_.wait_for(lk, std::chrono::milliseconds(intervalMs_),
                         [this] { return stop_.load(std::memory_order_acquire); });
        if (stop_.load(std::memory_order_acquire)) return;

        // 内容哈希检测（mtime 在 NTFS 对覆写不可靠，不作短路条件）
        if (!hasBaseline_) continue;
        if (auto content = readFile()) {
            if (fnv1a(*content) != lastHash_) {
                reloadLocked(); // 内容确实变化 → 重载
            }
        }
    }
}

} // namespace zenith::config
