// ============================================================================
//  io_uring 后端实现（Linux）
// ============================================================================
#include "zenith/net/poller_io_uring.hpp"

#include "zenith/core/platform.hpp"

#if defined(ZENITH_PLATFORM_LINUX)

#include <linux/io_uring.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <vector>

namespace zenith::net {
namespace {

int sysIouringSetup(unsigned entries, io_uring_params* p) {
    return static_cast<int>(::syscall(__NR_io_uring_setup, entries, p));
}

int sysIouringEnter(int fd, unsigned toSubmit, unsigned minComplete, unsigned flags) {
    return static_cast<int>(
        ::syscall(__NR_io_uring_enter, fd, toSubmit, minComplete, flags, nullptr, 0));
}

constexpr uint64_t kTimeoutTag = 0xFFFFFFFFFFFFFFFFull;

}  // namespace

bool ioUringAvailable() {
    static const bool ok = [] {
        io_uring_params p{};
        const int fd = sysIouringSetup(8, &p);
        if (fd < 0) return false;
        ::close(fd);
        return true;
    }();
    return ok;
}

class IoUringPoller final : public Poller {
public:
    explicit IoUringPoller(bool& ok) { ok = setup(); }
    ~IoUringPoller() override { teardown(); }

    bool registerFd(fd_t fd, uint16_t events) override {
        if (fd == kInvalidFd || !valid_) return false;
        want_[fd] = events;
        return arm(fd, events);
    }

    bool updateFd(fd_t fd, uint16_t events) override {
        if (fd == kInvalidFd || !valid_) return false;
        auto it = want_.find(fd);
        if (it == want_.end()) return false;
        it->second = events;
        return true;  // 下次事件到达时按新掩码重挂
    }

    bool deregisterFd(fd_t fd) override {
        if (!valid_) return false;
        want_.erase(fd);
        // 尽力撤掉常驻的 POLL_ADD；撤不掉也无妨（CQE 会被忽略）
        if (auto* sqe = getSqe()) {
            sqe->opcode = IORING_OP_POLL_REMOVE;
            sqe->fd = fd;
            sqe->addr = static_cast<uint64_t>(fd);  // 原 POLL_ADD 的 user_data
            sqe->user_data = kTimeoutTag - 1;       // 标记为"内部操作"
            commitSqe();
        }
        return true;
    }

    int poll(std::vector<PollResult>& out, int timeoutMs) override {
        if (!valid_) return 0;

        // 超时用 TIMEOUT 操作实现
        __kernel_timespec ts{};
        if (timeoutMs >= 0) {
            ts.tv_sec = timeoutMs / 1000;
            ts.tv_nsec = static_cast<long long>(timeoutMs % 1000) * 1000000LL;
            if (auto* sqe = getSqe()) {
                sqe->opcode = IORING_OP_TIMEOUT;
                sqe->fd = 0;
                sqe->addr = reinterpret_cast<uint64_t>(&ts);
                sqe->len = 1;
                sqe->user_data = kTimeoutTag;
                commitSqe();
            }
        }

        if (pendingSubmit_ > 0) {
            const unsigned n = pendingSubmit_;
            pendingSubmit_ = 0;
            (void)sysIouringEnter(ringFd_, n, 1, IORING_ENTER_GETEVENTS);
        }

        int count = 0;
        unsigned head = *cqHead_;
        while (head != *cqTail_) {
            io_uring_cqe* cqe = &cqes_[head & *cqRingMask_];
            const uint64_t tag = cqe->user_data;
            const int res = cqe->res;
            ++head;

            if (tag == kTimeoutTag || tag == kTimeoutTag - 1) continue;  // 计时器/内部操作

            const fd_t fd = static_cast<fd_t>(tag);
            auto it = want_.find(fd);
            if (it == want_.end()) continue;  // 已注销：丢弃过期事件

            uint16_t revents = kPollNone;
            if (res < 0) {
                revents |= kPollError;
            } else {
                if ((res & POLLIN) != 0) revents |= kPollReadable;
                if ((res & POLLOUT) != 0) revents |= kPollWritable;
                if ((res & POLLERR) != 0) revents |= kPollError;
                if ((res & (POLLHUP | POLLNVAL)) != 0) revents |= kPollHangup;
            }
            if (revents != kPollNone) {
                out.push_back(PollResult{fd, revents});
                ++count;
            }
            arm(fd, it->second);  // 重新挂上（poll op 是一次性的）
        }
        __atomic_store_n(cqHead_, head, __ATOMIC_RELEASE);
        return count;
    }

    [[nodiscard]] const char* name() const override { return "io_uring"; }

private:
    bool setup() {
        io_uring_params p{};
        ringFd_ = sysIouringSetup(kEntries, &p);
        if (ringFd_ < 0) return false;

        sqRingSize_ = p.sq_off.array + p.sq_entries * sizeof(unsigned);
        cqRingSize_ = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
        const bool singleMmap = (p.features & IORING_FEAT_SINGLE_MMAP) != 0;
        if (singleMmap && cqRingSize_ > sqRingSize_) sqRingSize_ = cqRingSize_;

        sqPtr_ = ::mmap(nullptr, sqRingSize_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                        ringFd_, IORING_OFF_SQ_RING);
        if (sqPtr_ == MAP_FAILED) return false;
        if (singleMmap) {
            cqPtr_ = sqPtr_;
        } else {
            cqPtr_ = ::mmap(nullptr, cqRingSize_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                            ringFd_, IORING_OFF_CQ_RING);
            if (cqPtr_ == MAP_FAILED) return false;
        }
        sqes_ = static_cast<io_uring_sqe*>(::mmap(nullptr, p.sq_entries * sizeof(io_uring_sqe),
                                                  PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                                                  ringFd_, IORING_OFF_SQES));
        if (sqes_ == MAP_FAILED) return false;

        auto* sq = static_cast<unsigned char*>(sqPtr_);
        sqHead_ = reinterpret_cast<unsigned*>(sq + p.sq_off.head);
        sqTail_ = reinterpret_cast<unsigned*>(sq + p.sq_off.tail);
        sqRingMask_ = reinterpret_cast<unsigned*>(sq + p.sq_off.ring_mask);
        sqRingEntries_ = reinterpret_cast<unsigned*>(sq + p.sq_off.ring_entries);
        sqArray_ = reinterpret_cast<unsigned*>(sq + p.sq_off.array);

        auto* cq = static_cast<unsigned char*>(cqPtr_);
        cqHead_ = reinterpret_cast<unsigned*>(cq + p.cq_off.head);
        cqTail_ = reinterpret_cast<unsigned*>(cq + p.cq_off.tail);
        cqRingMask_ = reinterpret_cast<unsigned*>(cq + p.cq_off.ring_mask);
        cqes_ = reinterpret_cast<io_uring_cqe*>(cq + p.cq_off.cqes);

        valid_ = true;
        return true;
    }

    void teardown() {
        if (sqes_ != nullptr && sqes_ != MAP_FAILED) {
            ::munmap(sqes_, kEntries * sizeof(io_uring_sqe));
            sqes_ = nullptr;
        }
        if (sqPtr_ != nullptr && sqPtr_ != MAP_FAILED) {
            ::munmap(sqPtr_, sqRingSize_);
            sqPtr_ = nullptr;
        }
        if (cqPtr_ != nullptr && cqPtr_ != MAP_FAILED && cqPtr_ != sqPtr_) {
            ::munmap(cqPtr_, cqRingSize_);
            cqPtr_ = nullptr;
        }
        if (ringFd_ >= 0) {
            ::close(ringFd_);
            ringFd_ = -1;
        }
        valid_ = false;
    }

    io_uring_sqe* getSqe() {
        const unsigned tail = *sqTail_;
        if (tail - *sqHead_ >= *sqRingEntries_) return nullptr;  // 环满
        const unsigned index = tail & *sqRingMask_;
        io_uring_sqe* sqe = &sqes_[index];
        std::memset(sqe, 0, sizeof(*sqe));
        sqArray_[index] = index;
        sqe->user_data = 0;
        return sqe;
    }

    /// 把刚填好的 SQE 发布给内核（tail 前进 + 计入待提交）。
    /// getSqe 只负责取槽位，**必须**紧跟 commitSqe，否则内核看不到这条 SQE。
    void commitSqe() {
        const unsigned tail = *sqTail_ + 1;
        __atomic_store_n(sqTail_, tail, __ATOMIC_RELEASE);
        pendingSubmit_++;
    }

    /// 提交一个 POLL_ADD（完成后 SQ tail 前进）
    bool arm(fd_t fd, uint16_t events) {
        auto* sqe = getSqe();
        if (sqe == nullptr) return false;
        sqe->opcode = IORING_OP_POLL_ADD;
        sqe->fd = fd;
        sqe->poll_events =
            static_cast<uint16_t>(((events & kPollReadable) ? POLLIN : 0) |
                                  ((events & kPollWritable) ? POLLOUT : 0) | POLLERR | POLLHUP);
        sqe->user_data = static_cast<uint64_t>(fd);
        commitSqe();
        return true;
    }

    static constexpr unsigned kEntries = 256;

    int ringFd_ = -1;
    void* sqPtr_ = nullptr;
    void* cqPtr_ = nullptr;
    io_uring_sqe* sqes_ = nullptr;
    size_t sqRingSize_ = 0;
    size_t cqRingSize_ = 0;

    unsigned* sqHead_ = nullptr;
    unsigned* sqTail_ = nullptr;
    unsigned* sqRingMask_ = nullptr;
    unsigned* sqRingEntries_ = nullptr;
    unsigned* sqArray_ = nullptr;

    unsigned* cqHead_ = nullptr;
    unsigned* cqTail_ = nullptr;
    unsigned* cqRingMask_ = nullptr;
    io_uring_cqe* cqes_ = nullptr;

    unsigned pendingSubmit_ = 0;
    bool valid_ = false;
    std::map<fd_t, uint16_t> want_;
};

std::unique_ptr<Poller> createIoUringPoller() {
    if (!ioUringAvailable()) return nullptr;
    bool ok = false;
    auto p = std::make_unique<IoUringPoller>(ok);
    if (!ok) return nullptr;
    return p;
}

}  // namespace zenith::net

#else  // 非 Linux：提供空实现，保持接口完整

namespace zenith::net {
bool ioUringAvailable() {
    return false;
}
std::unique_ptr<Poller> createIoUringPoller() {
    return nullptr;
}
}  // namespace zenith::net

#endif
