// ============================================================================
//  zenith::http::HttpClient —— HTTP/1.1 客户端（M4 增强：HTTPS + 连接池 + URL）
//  - 异步形态：request(...) + 回调（用户驱动 EventLoop）
//  - 阻塞形态：fetchBlocking(...)（内部起临时 loop，测试/CLI 友好）
//  - keep-alive 连接池：同 host:port 空闲连接复用（惰性剔除失效连接）
//  - https：req.tls 或 url scheme=https；默认系统信任根，可指定 CA 文件
// ============================================================================
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/core/result.hpp"
#include "zenith/http/http_parser.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/tcp_connection.hpp"
#include "zenith/tls/tls_context.hpp"

namespace zenith::http {

struct ClientRequest {
    std::string host = "127.0.0.1";
    uint16_t port = 80;
    HttpMethod method = HttpMethod::Get;
    std::string path = "/";
    HttpHeaders headers; // Host/Content-Length/Connection 由客户端自动填写
    std::string body;
    int timeoutMs = 8000;

    // ---- M4 扩展 ----
    std::string url;          // 非空则解析覆盖 host/port/path（支持 http/https）
    bool tls = false;         // scheme=https 时自动置位
    bool verifyPeer = true;   // https 证书校验
    std::string caFile;       // 额外 CA（PEM）；空则仅系统信任
    bool keepAlive = false;   // true 时请求走连接池复用（Connection: keep-alive）
};

class ZENITH_API HttpClient : private core::NonCopyable {
public:
    using ResponseCallback = std::function<void(core::Result<HttpResponse>)>;

    /// loop 由用户持有并运行；本对象生命周期需覆盖回调触发时刻。
    explicit HttpClient(net::EventLoop* loop);

    /// 注入自定义 TLS 上下文（默认按首个 https 请求的 verify/caFile 惰性创建）。
    void setTlsContext(std::shared_ptr<tls::OpenSslContext> ctx) { tlsCtx_ = std::move(ctx); }

    /// 发起请求（可在任意线程调用；连接与解析发生在 loop 线程）。
    void request(const ClientRequest& req, ResponseCallback cb);

    /// 阻塞便捷入口：内部创建临时 EventLoop 并在调用线程上运行。
    /// 超时/连接失败/协议错误统一走 Result::Err。
    static core::Result<HttpResponse> fetchBlocking(const ClientRequest& req);

    /// 阻塞便捷入口（URL 直填）。
    static core::Result<HttpResponse> fetchBlockingUrl(const std::string& url,
                                                       HttpMethod m = HttpMethod::Get,
                                                       std::string body = "");

    /// 清空连接池（关闭全部空闲连接）。
    void clearPool();

    [[nodiscard]] size_t pooledConnections() const;

private:
    struct AsyncState {
        HttpResponseParser parser;
        ResponseCallback cb;
        net::TcpConnection::Ptr conn;
        net::EventLoop* loop = nullptr;
        uint64_t timerId = 0;
        bool done = false;
        bool reusable = false; // 响应完成且 keep-alive → 归还池
        std::string poolKey;
    };

    void finishState(const std::shared_ptr<AsyncState>& st, core::Result<HttpResponse> r);
    void issueOn(const net::TcpConnection::Ptr& conn, const ClientRequest& req,
                 const std::shared_ptr<AsyncState>& st);
    void returnToPool(const std::shared_ptr<AsyncState>& st);
    std::shared_ptr<tls::OpenSslContext> tlsContextFor(const ClientRequest& req);

    net::EventLoop* loop_;
    std::shared_ptr<tls::OpenSslContext> tlsCtx_;
    // 连接池（key = "T?host:port"）；const 方法（pooledConnections）亦需读取
    mutable std::mutex poolMtx_;
    std::map<std::string, std::vector<net::TcpConnection::Ptr>> pool_;
    static constexpr size_t kMaxPoolPerHost = 8;
};

} // namespace zenith::http
