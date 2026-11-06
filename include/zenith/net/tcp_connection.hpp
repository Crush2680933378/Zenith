// ============================================================================
//  zenith::net::TcpConnection —— 单条 TCP 连接（生命周期由 shared_ptr 管理）
//  - 输入/输出双 Buffer；写不出去时自动挂写兴趣
//  - send() 线程安全（非 loop 线程自动投递）
//  - context()：附带任意状态（HTTP 层挂解析器等）
// ============================================================================
#pragma once

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

#include "zenith/core/buffer.hpp"
#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/socket.hpp"

namespace zenith::tls {
class TlsFilter;  // 实现见 zenith/tls/tls_context.hpp
}

namespace zenith::net {

class TcpConnection : private core::NonCopyable,
                      public std::enable_shared_from_this<TcpConnection> {
public:
    using Ptr = std::shared_ptr<TcpConnection>;
    using ConnectCallback = std::function<void(const Ptr&)>;
    using MessageCallback = std::function<void(const Ptr&, core::Buffer&)>;
    using CloseCallback = std::function<void(const Ptr&)>;
    using ErrorCallback = std::function<void(const Ptr&, const std::string&)>;

    static Ptr create(EventLoop* loop, fd_t fd, std::string peer) {
        return Ptr(new TcpConnection(loop, fd, std::move(peer)));
    }

    ~TcpConnection();

    /// 诊断：当前存活的连接数（排查「关闭后对象未析构」类问题，零成本量级）
    static int liveCount() noexcept;

    /* 建立后由创建方调用一次 */
    void establish();                        // 触发 onConnect；注册读兴趣
    void finalize(CloseCallback hostClose);  // 注入宿主清理（server 移表/client 收尾）

    void setConnectCallback(ConnectCallback cb) { onConnect_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb) { onMessage_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { onClose_ = std::move(cb); }
    void setErrorCallback(ErrorCallback cb) { onError_ = std::move(cb); }

    /* 数据发送（线程安全） */
    void send(const void* data, size_t n);
    void send(const std::string& s) { send(s.data(), s.size()); }

    /// M7.3：发送文件内容 [offset, offset+count)。
    /// POSIX（Linux/macOS）走 sendfile 内核态零拷贝；Windows 或 TLS 连接
    /// 自动回退为分块读发（内存占用有界，语义一致）。
    /// 与 send() 共用事件循环队列：字节顺序=调用顺序，文件正文一定排在
    /// 之前 send() 的响应头之后（期间新到的 send() 会缓冲到 outBuf_，
    /// 由 handleWrite 在文件发完后依次送出）。
    void sendFile(const std::string& path, uint64_t offset, uint64_t count);
    [[nodiscard]] bool fileSendActive() const noexcept {
        return file_ != nullptr || filePendingStart_;
    }

    /// 写半关闭：排空发送缓冲后 shutdown(WR)。
    void shutdown();
    /// 立即关闭。
    void forceClose();

    [[nodiscard]] EventLoop* loop() const noexcept { return loop_; }
    [[nodiscard]] fd_t fd() const noexcept { return fd_; }
    [[nodiscard]] const std::string& peer() const noexcept { return peer_; }
    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }
    [[nodiscard]] size_t pendingOutputBytes() const noexcept {
        return outBuf_.readableBytes() + postFileBuf_.readableBytes() +
               static_cast<size_t>(fileRemain_);
    }

    /* 附带状态（如 HTTP 解析器） */
    void setContext(std::shared_ptr<void> ctx) { context_ = std::move(ctx); }
    template <typename T>
    [[nodiscard]] std::shared_ptr<T> context() const {
        return std::static_pointer_cast<T>(context_);
    }

    /* ---- TLS 滤层（M4.1）----
     * 安装后：socket 密文进 → 解密后进入输入缓冲；send() 的明文自动加密。
     * 握手在后台推进；onConnect_ 在握手完成时触发（若已设置）。
     * 拥有 SSL* 的所有权。仅同 loop 线程调用。 */
    void startTls(std::unique_ptr<tls::TlsFilter> filter);
    [[nodiscard]] bool tlsActive() const noexcept { return tls_ != nullptr; }
    [[nodiscard]] bool tlsEstablished() const noexcept;

private:
    TcpConnection(EventLoop* loop, fd_t fd, std::string peer);

    void handleEvents(uint16_t revents);
    void handleRead();
    void handleWrite();
    void handleClose(const std::string& reason);
    void sendInLoop(const void* data, size_t n);
    void enableWriteInterest(bool on);
    // 文件零拷贝发送（M7.3，仅 loop 线程）
    void startFileSend(std::string path, uint64_t offset, uint64_t count);
    bool pumpFileSend();  // true=仍需可写事件；false=已完成或已关闭
    void closeFileSend() noexcept;
    // TLS 辅助（M4.1）
    void sockSend(const std::string& wire);
    bool pumpTls(const char* cipher, size_t n);

    EventLoop* loop_;
    fd_t fd_;
    std::string peer_;
    Socket sock_;  // 仅用于 fd 的系统调用封装（move-in 保活）

    core::Buffer inBuf_;
    core::Buffer outBuf_;
    // M7.3：文件发送期间由 send() 追加的字节。必须与 outBuf_ 分开——
    // outBuf_ 里装的是「文件分块的尾部」，而这里的字节要等整个文件发完才能发，
    // 否则新响应的字节会插进文件正文中间（协议损坏）。
    core::Buffer postFileBuf_;
    bool writing_ = false;          // 仅 loop 线程
    bool shutdownPending_ = false;  // 仅 loop 线程

    // M7.3：文件正文发送状态（仅 loop 线程）
    std::FILE* file_ = nullptr;      // 正在发送的文件；nullptr=无
    uint64_t fileRemain_ = 0;        // 尚待发送字节数
    uint64_t fileOffset_ = 0;        // 当前文件偏移（POSIX sendfile 用）
    bool filePendingStart_ = false;  // 头部仍在 outBuf_，排空后再启动文件

    ConnectCallback onConnect_;
    MessageCallback onMessage_;
    CloseCallback onClose_;
    ErrorCallback onError_;
    CloseCallback hostClose_;

    std::unique_ptr<tls::TlsFilter> tls_;  // 可选 TLS 滤层（M4.1）
    std::shared_ptr<void> context_;
    std::atomic<bool> connected_{false};
};

}  // namespace zenith::net
