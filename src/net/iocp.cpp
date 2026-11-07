// ============================================================================
//  IOCP 传输层实现（Windows）
// ============================================================================
#include "zenith/net/iocp.hpp"

#if defined(ZENITH_PLATFORM_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// 顺序要紧：mswsock.h 依赖 winsock2.h + windows.h 里的类型定义，
// 先包含它（MSVC 的 mswsockdef.h 会报 unknown override specifier）。
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <mswsock.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace zenith::net {
namespace {

constexpr int kConcurrentAccepts = 4;
constexpr size_t kRecvBufSize = 4096;
constexpr DWORD kSockAddrLen = sizeof(sockaddr_in) + 16;

struct OverlappedEx {
    OVERLAPPED ov{};
    enum class Kind { Accept, Recv, Send, Quit } kind = Kind::Recv;
    SOCKET sock = INVALID_SOCKET;
    uint64_t connId = 0;
    std::string sendData;
    char buf[kRecvBufSize]{};
};

}  // namespace

struct IocpServer::Impl {
    HANDLE iocp = nullptr;
    SOCKET listenSock = INVALID_SOCKET;
    uint16_t port = 0;
    std::atomic<bool> running{false};
    std::vector<std::thread> workers;
    DataCallback onData;
    CloseCallback onClose;

    std::mutex mtx;
    std::map<uint64_t, SOCKET> conns;
    uint64_t nextId = 1;

    LPFN_ACCEPTEX acceptEx = nullptr;
    LPFN_GETACCEPTEXSOCKADDRS getAcceptAddr = nullptr;

    void postAccept();
    void postRecv(uint64_t id, SOCKET s);
    void closeConn(uint64_t id, SOCKET s);
    void workerLoop();
    void shutdown();
};

void IocpServer::Impl::postAccept() {
    auto* ex = new OverlappedEx();
    ex->kind = OverlappedEx::Kind::Accept;
    SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        delete ex;
        return;
    }
    ex->sock = client;
    DWORD bytes = 0;
    // 每次 accept 用一个临时 buffer（长度 2*(addr+16)，AcceptEx 要求）
    auto* addrBuf = new char[2 * kSockAddrLen];
    ex->sendData.assign(addrBuf, 2 * kSockAddrLen);
    delete[] addrBuf;
    if (!acceptEx(listenSock, client, &ex->sendData[0], 0, kSockAddrLen, kSockAddrLen, &bytes,
                  &ex->ov)) {
        if (::WSAGetLastError() != ERROR_IO_PENDING) {
            ::closesocket(client);
            delete ex;
        }
    }
}

void IocpServer::Impl::postRecv(uint64_t id, SOCKET s) {
    auto* ex = new OverlappedEx();
    ex->kind = OverlappedEx::Kind::Recv;
    ex->sock = s;
    ex->connId = id;
    WSABUF buf{};
    buf.buf = ex->buf;
    buf.len = static_cast<ULONG>(kRecvBufSize);
    DWORD flags = 0;
    DWORD received = 0;
    if (::WSARecv(s, &buf, 1, &received, &flags, &ex->ov, nullptr) == SOCKET_ERROR) {
        if (::WSAGetLastError() != WSA_IO_PENDING) {
            delete ex;
            closeConn(id, s);
        }
    }
}

void IocpServer::Impl::closeConn(uint64_t id, SOCKET s) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = conns.find(id);
        if (it != conns.end() && it->second == s) {
            conns.erase(it);
            notify = true;
        }
    }
    ::closesocket(s);
    if (notify && onClose) onClose(id);
}

void IocpServer::Impl::workerLoop() {
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;
        const BOOL ok = ::GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, INFINITE);
        if (ov == nullptr) {
            if (!running.load()) return;
            continue;
        }
        auto* ex = reinterpret_cast<OverlappedEx*>(ov);
        if (ex->kind == OverlappedEx::Kind::Quit) {
            delete ex;
            return;
        }
        if (ex->kind == OverlappedEx::Kind::Accept) {
            if (ok) {
                ::setsockopt(ex->sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                             reinterpret_cast<const char*>(&listenSock), sizeof(listenSock));
                if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(ex->sock), iocp, 0, 0) ==
                    nullptr) {
                    ::closesocket(ex->sock);
                } else {
                    uint64_t id = 0;
                    {
                        std::lock_guard<std::mutex> lk(mtx);
                        id = nextId++;
                        conns[id] = ex->sock;
                    }
                    postRecv(id, ex->sock);
                }
            } else {
                ::closesocket(ex->sock);
            }
            delete ex;
            if (running.load()) postAccept();
            continue;
        }
        if (ex->kind == OverlappedEx::Kind::Recv) {
            const uint64_t id = ex->connId;
            const SOCKET s = ex->sock;
            if (ok && bytes > 0) {
                if (onData) onData(id, ex->buf, bytes);
                delete ex;
                postRecv(id, s);
            } else {
                delete ex;
                closeConn(id, s);
            }
            continue;
        }
        // Send：做完就释放
        delete ex;
    }
}

void IocpServer::Impl::shutdown() {
    running.store(false);
    if (iocp != nullptr) {
        for (size_t i = 0; i < workers.size(); ++i) {
            auto* ex = new OverlappedEx();
            ex->kind = OverlappedEx::Kind::Quit;
            ::PostQueuedCompletionStatus(iocp, 0, 0, &ex->ov);
        }
    }
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
    workers.clear();
    {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& [id, s] : conns) {
            (void)id;
            ::closesocket(s);
        }
        conns.clear();
    }
    if (listenSock != INVALID_SOCKET) {
        ::closesocket(listenSock);
        listenSock = INVALID_SOCKET;
    }
    if (iocp != nullptr) {
        ::CloseHandle(iocp);
        iocp = nullptr;
    }
}

IocpServer::IocpServer() : impl_(std::make_unique<Impl>()) {}
IocpServer::~IocpServer() {
    stop();
    impl_->shutdown();
}

bool IocpServer::supported() {
    return true;
}

bool IocpServer::listen(const std::string& host, uint16_t port) {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    impl_->iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (impl_->iocp == nullptr) return false;

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    BOOL reuse = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::closesocket(s);
        return false;
    }
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(s, SOMAXCONN) != 0) {
        ::closesocket(s);
        return false;
    }
    if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), impl_->iocp, 0, 0) == nullptr) {
        ::closesocket(s);
        return false;
    }

    GUID acceptGuid = WSAID_ACCEPTEX;
    DWORD bytes = 0;
    if (::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &acceptGuid, sizeof(acceptGuid),
                   &impl_->acceptEx, sizeof(impl_->acceptEx), &bytes, nullptr, nullptr) != 0) {
        ::closesocket(s);
        return false;
    }
    impl_->listenSock = s;

    sockaddr_in bound{};
    int len = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        impl_->port = ::ntohs(bound.sin_port);
    }
    return true;
}

uint16_t IocpServer::port() const noexcept {
    return impl_->port;
}

bool IocpServer::start(DataCallback onData, CloseCallback onClose, int workers) {
    if (impl_->iocp == nullptr || impl_->listenSock == INVALID_SOCKET) return false;
    impl_->onData = std::move(onData);
    impl_->onClose = std::move(onClose);
    impl_->running.store(true);
    for (int i = 0; i < kConcurrentAccepts; ++i)
        impl_->postAccept();
    for (int i = 0; i < workers; ++i) {
        impl_->workers.emplace_back([this] { impl_->workerLoop(); });
    }
    return true;
}

bool IocpServer::send(uint64_t connId, std::string data) {
    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        auto it = impl_->conns.find(connId);
        if (it == impl_->conns.end()) return false;
        s = it->second;
    }
    auto* ex = new OverlappedEx();
    ex->kind = OverlappedEx::Kind::Send;
    ex->sock = s;
    ex->connId = connId;
    ex->sendData = std::move(data);
    WSABUF buf{};
    buf.buf = const_cast<char*>(ex->sendData.data());
    buf.len = static_cast<ULONG>(ex->sendData.size());
    DWORD sent = 0;
    if (::WSASend(s, &buf, 1, &sent, 0, &ex->ov, nullptr) == SOCKET_ERROR) {
        if (::WSAGetLastError() != WSA_IO_PENDING) {
            delete ex;
            return false;
        }
    }
    return true;
}

void IocpServer::stop() {
    if (!impl_->running.exchange(false)) return;
    // 关闭监听：让挂起的 AcceptEx 完成并失败
    if (impl_->listenSock != INVALID_SOCKET) {
        ::closesocket(impl_->listenSock);
        impl_->listenSock = INVALID_SOCKET;
    }
}

size_t IocpServer::connectionCount() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->conns.size();
}

}  // namespace zenith::net

#else  // 非 Windows

namespace zenith::net {

struct IocpServer::Impl {};

IocpServer::IocpServer() : impl_(std::make_unique<Impl>()) {}
IocpServer::~IocpServer() = default;

bool IocpServer::supported() {
    return false;
}
bool IocpServer::listen(const std::string&, uint16_t) {
    return false;
}
uint16_t IocpServer::port() const noexcept {
    return 0;
}
bool IocpServer::start(DataCallback, CloseCallback, int) {
    return false;
}
bool IocpServer::send(uint64_t, std::string) {
    return false;
}
void IocpServer::stop() {}
size_t IocpServer::connectionCount() const {
    return 0;
}

}  // namespace zenith::net

#endif
