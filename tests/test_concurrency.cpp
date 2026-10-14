// concurrency 模块单测：MpscRingQueue / ThreadPool
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "ztest.hpp"

#include "zenith/concurrency/mpsc_queue.hpp"
#include "zenith/concurrency/thread_pool.hpp"

using namespace zenith::concurrency;

/* ---------------- MpscRingQueue ---------------- */

ZTEST(MpscQueue, FifoSingleThread) {
    MpscRingQueue<int, 8> q;
    for (int i = 0; i < 8; ++i) ZEXPECT_TRUE(q.push(i));
    ZEXPECT_FALSE(q.push(99)); // 满
    for (int i = 0; i < 8; ++i) {
        int v = -1;
        ZEXPECT_TRUE(q.pop(v));
        ZEXPECT_EQ(v, i); // 严格 FIFO
    }
    int v;
    ZEXPECT_FALSE(q.pop(v)); // 空
}

ZTEST(MpscQueue, Wraparound) {
    MpscRingQueue<int, 4> q;
    for (int round = 0; round < 1000; ++round) {
        int v = -1;
        ZEXPECT_TRUE(q.push(round));
        ZEXPECT_TRUE(q.pop(v));
        ZEXPECT_EQ(v, round);
    }
}

ZTEST(MpscQueue, ConcurrentProducersNoLossNoDup) {
    MpscRingQueue<uint64_t, 4096> q;
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 20000;

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                const uint64_t v = static_cast<uint64_t>(p) * kPerProducer + static_cast<uint64_t>(i);
                while (!q.push(v)) std::this_thread::yield(); // 自旋重试
            }
        });
    }

    std::vector<std::atomic<int>> counts(kProducers * kPerProducer);
    for (auto& c : counts) c.store(0, std::memory_order_relaxed);

    uint64_t popped = 0;
    uint64_t v;
    while (popped < static_cast<uint64_t>(kProducers) * kPerProducer) {
        if (q.pop(v)) {
            counts[static_cast<size_t>(v)].fetch_add(1, std::memory_order_relaxed);
            ++popped;
        } else {
            std::this_thread::yield();
        }
    }
    for (auto& t : producers) t.join();

    for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i].load() != 1) {
            ztest::reportFail(__FILE__, __LINE__,
                              "item seen != 1 time: " + std::to_string(i));
        }
    }
}

/* ---------------- ThreadPool ---------------- */

ZTEST(ThreadPool, ExecutesAllTasks) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    constexpr int kTasks = 500;

    std::promise<void> done;
    auto doneFuture = done.get_future();
    for (int i = 0; i < kTasks; ++i) {
        ZEXPECT_TRUE(pool.post([&] { counter.fetch_add(1, std::memory_order_relaxed); }));
    }
    pool.post([&] { done.set_value(); });

    ZEXPECT_TRUE(doneFuture.wait_for(std::chrono::seconds(10)) ==
                 std::future_status::ready);
    ZEXPECT_EQ(counter.load(), kTasks);
}

ZTEST(ThreadPool, PostAfterShutdownFails) {
    ThreadPool pool(2);
    pool.shutdown();
    ZEXPECT_FALSE(pool.post([] {}));
    pool.shutdown(); // 幂等
}

ZTEST(ThreadPool, DestructorDrainsQueue) {
    std::mutex mtx;
    std::vector<int> done;
    {
        ThreadPool pool(2);
        for (int i = 0; i < 200; ++i) {
            pool.post([&mtx, &done, i] {
                std::lock_guard<std::mutex> lk(mtx);
                done.push_back(i);
            });
        }
    } // 析构：排空队列后 join
    ZEXPECT_EQ(done.size(), size_t{200});
}

ZTEST(ThreadPool, DefaultThreadCount) {
    ThreadPool pool; // hardware_concurrency 自动
    ZEXPECT_TRUE(pool.threadCount() > 0);
}

ZTEST_MAIN
