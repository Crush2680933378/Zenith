#include "zenith/http/http_server.hpp"

#include "zenith/log/logger.hpp"

namespace zenith::http {

namespace {
// 每连接状态：解析器（挂 connection context）
struct ConnState {
    explicit ConnState(size_t maxBody) : parser(maxBody) {}
    HttpRequestParser parser;
};
} // namespace

HttpServer::HttpServer(net::EventLoop* loop, net::EventLoopThreadPool* ioPool, size_t maxBody)
    : loop_(loop), server_(loop, ioPool), maxBodyBytes_(maxBody) {
    server_.setMessageCallback(
        [this](const net::TcpConnection::Ptr& conn, core::Buffer& buf) {
            onMessage(conn, buf);
        });
}

HttpServer::~HttpServer() { stop(); }

core::Result<void> HttpServer::listen(const std::string& host, uint16_t port, int backlog) {
    return server_.listen(host, port, backlog);
}

core::Result<void> HttpServer::listenTls(const tls::OpenSslContext& ctx,
                                          const std::string& host, uint16_t port, int backlog) {
    server_.setConnectionDecorator([&ctx](const net::TcpConnection::Ptr& conn) {
        SSL* ssl = ctx.newServerSession();
        if (ssl == nullptr) {
            conn->forceClose();
            return;
        }
        conn->startTls(std::make_unique<tls::TlsFilter>(/*serverSide=*/true, ssl));
    });
    return server_.listen(host, port, backlog);
}

void HttpServer::stop() { server_.stop(); }

void HttpServer::beginGracefulShutdown(std::function<void()> onDrained) {
    closing_.store(true);
    drainedCb_ = std::move(onDrained);
    server_.stop(); // 停 accept；既有连接继续服务至排空
    if (inflight_.load() == 0 && drainedCb_) {
        loop_->runInLoop([cb = std::move(drainedCb_)] { cb(); });
        drainedCb_ = nullptr;
    }
}

void HttpServer::maybeFireDrained() {
    inflight_.fetch_sub(1);
    if (closing_.load() && inflight_.load() <= 0 && drainedCb_) {
        loop_->runInLoop([cb = std::move(drainedCb_)] { cb(); });
        drainedCb_ = nullptr;
    }
}

void HttpServer::enableWebSocket(
    const std::string& path, std::function<void(const net::WebSocketSession::Ptr&)> onOpen) {
    wsPaths_[path] = std::move(onOpen);
}

bool HttpServer::tryWebSocketUpgrade(const net::TcpConnection::Ptr& conn) {
    auto state = conn->context<ConnState>();
    const std::string path = state->parser.request().path; // 先拷贝（reset 会清空内部请求）
    const std::string key = state->parser.request().headers.get("Sec-WebSocket-Key");
    if (key.empty()) return false;

    const std::string accept = net::wsComputeAccept(key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept + "\r\n\r\n";
    conn->send(resp);

    // 升级请求可能与客户端首帧同包到达：先取出残留再重置解析器。
    // 接管动作投递执行：takeover 会替换正在执行的 HTTP 消息回调，
    // 在其闭包内继续操作属于 use-after-free。
    const std::string earlyFrames = state->parser.releaseLeftover();
    state->parser.reset(); // HTTP 阶段结束
    auto* self = this;
    loop_->queueInLoop([self, conn, path, earlyFrames]() mutable {
        auto session = net::WebSocketSession::takeover(conn, /*clientSide=*/false);
        auto it = self->wsPaths_.find(path);
        if (it != self->wsPaths_.end() && it->second) it->second(session);
        session->processPending(earlyFrames); // 回放同包到达的早期帧
    });
    return true;
}

void HttpServer::onMessage(const net::TcpConnection::Ptr& conn, core::Buffer& buf) {
    auto state = conn->context<ConnState>();
    if (state == nullptr) {
        state = std::make_shared<ConnState>(maxBodyBytes_);
        conn->setContext(state);
    }

    for (;;) {
        const ParseOutcome out = state->parser.feed(buf); // buf 字节全部转入解析器
        if (out.failed()) {
            HttpResponse resp;
            resp.status = 400;
            resp.headers.set("Content-Type", "text/plain; charset=utf-8");
            resp.body = "400 Bad Request: " + out.error;
            conn->send(resp.serialize(true, false));
            conn->shutdown();
            return;
        }
        if (!out.complete()) return; // 等待更多数据

        handleRequest(conn);

        // pipelining：取出残留字节（可能是下一条请求的开头）
        std::string leftover = state->parser.releaseLeftover();
        state->parser.reset();
        if (leftover.empty()) return;
        buf.appendString(leftover);
    }
}

void HttpServer::handleRequest(const net::TcpConnection::Ptr& conn) {
    inflight_.fetch_add(1);
    auto inflightGuard = [this](void*) { maybeFireDrained(); };
    std::unique_ptr<void, decltype(inflightGuard)> guard{reinterpret_cast<void*>(this), inflightGuard};

    auto state = conn->context<ConnState>();
    HttpRequest req = state->parser.request();
    req.peer = conn->peer();

    // WebSocket 升级路径（先于路由）
    if (req.method == HttpMethod::Get &&
        core::string_util::toLower(req.headers.get("Upgrade")) == "websocket" &&
        core::string_util::toLower(req.headers.get("Connection")).find("upgrade") !=
            std::string::npos &&
        wsPaths_.count(req.path) > 0) {
        if (tryWebSocketUpgrade(conn)) return;
    }

    Context ctx(std::move(req));
    ctx.response().httpMajor = ctx.request().httpMajor;
    ctx.response().httpMinor = ctx.request().httpMinor;

    try {
        router_.dispatch(ctx);
    } catch (const Context::BadRequestBody& e) {
        ctx.status(400).text(400, std::string("400 Bad Request: ") + e.what());
    } catch (const std::exception& e) {
        ZLOG_ERROR("http handler exception: ", e.what(), " (", ctx.peer(), " ",
                   ctx.request().path, ")");
        ctx.status(500).text(500, std::string("500 Internal Server Error"));
    } catch (...) {
        ZLOG_ERROR("http handler unknown exception (", ctx.peer(), " ", ctx.request().path, ")");
        ctx.status(500).text(500, "500 Internal Server Error");
    }

    // 未显式写响应体的合法情形：handler 未设置 body → 200 空体
    const bool isHead = ctx.request().method == HttpMethod::Head;
    bool keep = ctx.request().keepAlive();
    if (closing_.load()) keep = false; // 停机中：一律短连接
    const std::string wire = ctx.response().serialize(/*includeBody=*/!isHead, keep);
    conn->send(wire);
    if (!keep) conn->shutdown();
}

} // namespace zenith::http
