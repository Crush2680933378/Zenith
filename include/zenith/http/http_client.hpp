// ============================================================================
//  zenith::http::HttpClient —— 基于 EventLoop 的 HTTP/1.1 客户端
//  - 异步形态：request(...) + 回调（用户驱动 EventLoop）
//  - 阻塞形态：fetchBlocking(...)（内部起临时 loop，测试/CLI 友好）
//  - M2 策略：每请求一条连接（无连接池；keep-alive 池后续里程碑）
// ============================================================================
#pragma once

#include <functional>
#include <memory>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/core/result.hpp"
#include "zenith/http/http_parser.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/tcp_connection.hpp"

namespace zenith::http {

struct ClientRequest {
    std::string host = "127.0.0.1";
    uint16_t port = 80;
    HttpMethod method = HttpMethod::Get;
    std::string path = "/";
    HttpHeaders headers; // Host/Content-Length/Connection 由客户端自动填写
    std::string body;
    int timeoutMs = 8000;
};

class ZENITH_API HttpClient : private core::NonCopyable {
public:
    using ResponseCallback = std::function<void(core::Result<HttpResponse>)>;

    /// loop 由用户持有并运行；本对象生命周期需覆盖回调触发时刻。
    explicit HttpClient(net::EventLoop* loop);

    /// 发起请求（可在任意线程调用；连接与解析发生在 loop 线程）。
    void request(const ClientRequest& req, ResponseCallback cb);

    /// 阻塞便捷入口：内部创建临时 EventLoop 并在调用线程上运行。
    /// 超时/连接失败/协议错误统一走 Result::Err。
    static core::Result<HttpResponse> fetchBlocking(const ClientRequest& req);

private:
    struct AsyncState {
        HttpResponseParser parser;
        ResponseCallback cb;
        net::TcpConnection::Ptr conn;
        net::EventLoop* loop = nullptr;
        uint64_t timerId = 0;
        bool done = false;
    };

    static void finishState(const std::shared_ptr<AsyncState>& st,
                            core::Result<HttpResponse> r);

    net::EventLoop* loop_;
};

} // namespace zenith::http
