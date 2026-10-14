// ============================================================================
//  MpscRingQueue —— 有界无锁环形队列（多生产者 / 单消费者）
//  基于 Vyukov 有界 MPMC 算法（对 MPSC 是安全超集）：
//    - 生产者对 tail_ 做 CAS；消费者独占 head_
//    - 每个 slot 用序号 turn 标记可写/可读，避免ABA
//  约束：
//    - T 需可默认构造、可移动赋值
//    - 队列满时 push 返回 false（调用方自行决定丢弃/重试）
//    - 消费端为单线程；多消费者同样正确（算法本身支持 MPMC）
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace zenith::concurrency {

// 注：GCC 的 std::hardware_destructive_interference_size 非严格常量表达式
// （-Winterference-size），为可移植性直接固定 64。
inline constexpr size_t kCacheLine = 64;

template <typename T, size_t Capacity>
class MpscRingQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity 必须是 2 的幂");

public:
    MpscRingQueue() noexcept {
        for (size_t i = 0; i < Capacity; ++i) slots_[i].turn.store(i, std::memory_order_relaxed);
    }

    MpscRingQueue(const MpscRingQueue&) = delete;
    MpscRingQueue& operator=(const MpscRingQueue&) = delete;

    /// 多线程安全。返回 false 表示队列已满。
    bool push(T value) noexcept(std::is_nothrow_move_assignable_v<T>) {
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & kMask];
            const size_t turn = slot.turn.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(turn) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.value = std::move(value);
                    slot.turn.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS 失败：pos 已被更新为最新值，继续循环
            } else if (dif < 0) {
                // 该 slot 尚被上一轮占用 → 队列满
                return false;
            } else {
                // 其他生产者已推进 tail，重新读取
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    /// 仅单消费者线程调用。返回 false 表示队列空。
    bool pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const size_t pos = head_.load(std::memory_order_relaxed);
        Slot& slot = slots_[pos & kMask];
        const size_t turn = slot.turn.load(std::memory_order_acquire);
        const intptr_t dif = static_cast<intptr_t>(turn) - static_cast<intptr_t>(pos + 1);
        if (dif != 0) return false; // 尚未写入完成

        head_.store(pos + 1, std::memory_order_relaxed);
        out = std::move(slot.value);
        slot.turn.store(pos + Capacity, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool emptyApprox() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

private:
    struct Slot {
        std::atomic<size_t> turn{0};
        T value{};
    };

    static constexpr size_t kMask = Capacity - 1;

    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    alignas(kCacheLine) std::atomic<size_t> head_{0};
    alignas(kCacheLine) Slot slots_[Capacity];
};

} // namespace zenith::concurrency
