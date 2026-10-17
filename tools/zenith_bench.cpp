// ============================================================================
//  zenith-bench —— 自研 HTTP 压测工具（M5.4）
//  用法：
//    zenith-bench -u http://127.0.0.1:8080/api [-d 10] [-c 4] [-t 4] [--tls-ca ca.crt]
//  输出：RPS / P50 / P90 / P99 / 错误数 / 字节吞吐（markdown 表）
//  实现：N 客户端线程 × 每 HttpClient keep-alive 池串行链式请求（复用 M4 连接池）
// ============================================================================
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "zenith/http/http_client.hpp"
#include "zenith/log/logger.hpp"

using namespace zenith;

namespace {

struct Args {
    std::string url = "http://127.0.0.1:8080/";
    int durationSec = 5;
    int connsPerThread = 4; // 每线程并发链数
    int threads = 2;
    std::string caFile;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (s == "-u") a.url = next();
        else if (s == "-d") a.durationSec = std::atoi(next());
        else if (s == "-c") a.connsPerThread = std::atoi(next());
        else if (s == "-t") a.threads = std::atoi(next());
        else if (s == "--tls-ca") a.caFile = next();
    }
    return a;
}

struct Stats {
    std::atomic<uint64_t> ok{0};
    std::atomic<uint64_t> fail{0};
    std::atomic<uint64_t> bytes{0};
    // 延迟直方图（毫秒桶）
    static constexpr size_t kBuckets = 12;
    std::atomic<uint64_t> latMs[kBuckets]{};
    static constexpr int64_t kBoundsMs[kBuckets] = {1, 2, 5, 10, 20, 50, 100, 200, 500,
                                                    1000, 5000, 9223372036854775807LL};
    void record(int64_t us, uint64_t bodyBytes) {
        bytes.fetch_add(bodyBytes, std::memory_order_relaxed);
        const int64_t ms = us / 1000;
        for (size_t i = 0; i < kBuckets; ++i) {
            if (ms <= kBoundsMs[i]) {
                latMs[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    [[nodiscard]] double percentile(double q) const {
        uint64_t total = 0;
        for (auto& b : latMs) total += b.load();
        if (total == 0) return 0;
        const double target = static_cast<double>(total) * q;
        uint64_t cum = 0;
        for (size_t i = 0; i < kBuckets; ++i) {
            cum += latMs[i].load();
            if (static_cast<double>(cum) >= target) {
                return static_cast<double>(kBoundsMs[i]) / 1000.0;
            }
        }
        return 5.0;
    }
};

} // namespace

int main(int argc, char** argv) {
    Args a = parseArgs(argc, argv);
    log::LoggerFacade::instance().clearSinks(); // 压测静音

    std::printf("zenith-bench\n  url=%s dur=%ds threads=%d chains/thread=%d\n", a.url.c_str(),
                a.durationSec, a.threads, a.connsPerThread);

    Stats st;
    std::atomic<bool> stop{false};
    std::vector<std::thread> pool;

    for (int t = 0; t < a.threads; ++t) {
        pool.emplace_back([&, t] {
            net::EventLoop loop;
            std::thread loopThread([&loop] { loop.loop(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            std::vector<std::shared_ptr<http::HttpClient>> clients;
            for (int c = 0; c < a.connsPerThread; ++c) {
                clients.push_back(std::make_shared<http::HttpClient>(&loop));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // 每链一个递归闭包（捕获自身的 shared_ptr）
            struct Chain {
                http::HttpClient* cl;
                net::EventLoop* loop;
                const Args* a;
                Stats* st;
                std::atomic<bool>* stop;
                std::function<void()> step;

                void run() {
                    if (stop->load()) return;
                    http::ClientRequest rq;
                    rq.url = a->url;
                    rq.keepAlive = true;
                    rq.timeoutMs = 8000;
                    rq.caFile = a->caFile;
                    const int64_t t0 = core::steadyMicroseconds();
                    loop->queueInLoop([this, rq, t0]() {
                        cl->request(rq, [this, t0](core::Result<http::HttpResponse> r) {
                            if (r && r.value().status == 200) {
                                st->ok.fetch_add(1, std::memory_order_relaxed);
                                st->record(core::steadyMicroseconds() - t0,
                                           r.value().body.size());
                            } else {
                                st->fail.fetch_add(1, std::memory_order_relaxed);
                            }
                            step();
                        });
                    });
                }
            };

            auto chains = std::make_shared<std::vector<std::shared_ptr<Chain>>>();
            for (int c = 0; c < a.connsPerThread; ++c) {
                auto ch = std::make_shared<Chain>();
                ch->cl = clients[static_cast<size_t>(c)].get();
                ch->loop = &loop;
                ch->a = &a;
                ch->st = &st;
                ch->stop = &stop;
                ch->step = [ch] { ch->run(); };
                chains->push_back(ch);
            }
            for (auto& ch : *chains) loop.queueInLoop([ch] { ch->run(); });

            std::this_thread::sleep_for(std::chrono::seconds(a.durationSec));
            stop.store(true);
            // 等在途回调收敛（最多 2s）
            for (int i = 0; i < 200; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            (void)0;
            loop.stop();
            loopThread.join();
        });
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (auto& t : pool) t.join();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                            .count();

    const uint64_t total = st.ok.load() + st.fail.load();
    const double rps = static_cast<double>(st.ok.load()) / wall;

    std::printf("\n| metric | value |\n|---|---|\n");
    std::printf("| duration | %.2fs |\n", wall);
    std::printf("| requests(ok) | %llu |\n", static_cast<unsigned long long>(st.ok.load()));
    std::printf("| requests(fail) | %llu |\n", static_cast<unsigned long long>(st.fail.load()));
    std::printf("| rps | %.0f |\n", rps);
    std::printf("| p50 | %.3fs |\n", st.percentile(0.50));
    std::printf("| p90 | %.3fs |\n", st.percentile(0.90));
    std::printf("| p99 | %.3fs |\n", st.percentile(0.99));
    std::printf("| body_throughput | %.1f KB/s |\n",
                static_cast<double>(st.bytes.load()) / 1024.0 / wall);
    std::printf("| total | %llu |\n", static_cast<unsigned long long>(total));
    return 0;
}
