#include "zenith/http/http_server.hpp"

#include "zenith/http/http2.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/obs/trace_context.hpp"

namespace zenith::http {

namespace {
// 每连接状态：解析器（挂 connection context）
struct ConnState {
    explicit ConnState(size_t maxBody) : parser(maxBody) {}
    HttpRequestParser parser;
    bool streaming = false;  // M9.1：流式响应在途（不参与空闲踢除/keep-alive）
    // M7.2a：连接级 Context 池——keep-alive 每请求零构造（reset 复用堆容量）
    std::unique_ptr<Context> ctx;
    // M7.7：h2c（prior knowledge）连接状态；非空即整条连接走 HTTP/2
    std::unique_ptr<h2::H2ServerConnection> h2;
};
}  // namespace

HttpServer::HttpServer(net::EventLoop* loop, net::EventLoopThreadPool* ioPool, size_t maxBody)
    : HttpServer(loop, ioPool, [&] {
          SecurityLimits l;
          l.maxBodyBytes = maxBody;
          return l;
      }()) {}

HttpServer::HttpServer(net::EventLoop* loop, net::EventLoopThreadPool* ioPool,
                       const SecurityLimits& limits)
    : loop_(loop), server_(loop, ioPool), limits_(limits) {
    maxBodyBytes_ = limits.maxBodyBytes;
    server_.setMessageCallback(
        [this](const net::TcpConnection::Ptr& conn, core::Buffer& buf) { onMessage(conn, buf); });
    // 连接装饰器：计数 + 空闲踢除定时器（slowloris 防线）
    server_.addConnectionDecorator([this](const net::TcpConnection::Ptr& conn) {
        const int n = connCount_.fetch_add(1) + 1;
        if (static_cast<size_t>(n) > limits_.maxConcurrentConns) {
            // 装饰器在 establish 前执行：send/forceClose 此刻会被静默丢弃——
            // 推迟到连接建立回调（connected_ 已置位）再拒绝
            conn->setConnectCallback([](const net::TcpConnection::Ptr& c) {
                HttpResponse resp;
                resp.status = 503;
                resp.headers.set("Content-Type", "text/plain; charset=utf-8");
                resp.body = "503 connection limit exceeded";
                c->send(resp.serialize(true, false));
                c->forceClose();
            });
            return;
        }
        // keep-alive 空闲超时：连接无请求超限即关（有在途请求则续期）
        armIdleTimer(conn);
        conn->setCloseCallback([this](const net::TcpConnection::Ptr& c) {
            connCount_.fetch_sub(1);
            cancelIdleTimer(c);
        });
    });
}

void HttpServer::armIdleTimer(const net::TcpConnection::Ptr& conn) {
    if (limits_.keepAliveIdleSec <= 0) return;
    const auto key = reinterpret_cast<uintptr_t>(conn.get());
    std::lock_guard<std::mutex> lk(idleMtx_);
    // 每个请求都会重新 arm：先撤掉上一个，否则旧定时器仍排在队列里，
    // 既重复计时，又（配合下面的强捕获）把连接多吊 keepAliveIdleSec 秒。
    const auto it = idleTimers_.find(key);
    if (it != idleTimers_.end()) {
        loop_->cancel(it->second);
        idleTimers_.erase(it);
    }
    // 弱捕获：定时器不该拥有连接。强捕获会让「已关闭但定时器未到期」的连接
    // 一直活着（LSan 报的 TcpConnection 缓冲泄漏就是这么来的）。
    std::weak_ptr<net::TcpConnection> wconn = conn;
    idleTimers_[key] =
        loop_->scheduleAfter(std::chrono::seconds(limits_.keepAliveIdleSec), [this, wconn, key] {
            std::lock_guard<std::mutex> lk2(idleMtx_);
            idleTimers_.erase(key);
            auto conn = wconn.lock();
            if (!conn || !conn->connected() || inflight_.load() > 0) return;
            // M9.1：SSE/分块流在途时不得踢除——长连接本身可能长时间无新字节
            auto st = conn->context<ConnState>();
            if (st != nullptr && st->streaming) return;
            conn->forceClose();  // 空闲超限
        });
}

void HttpServer::cancelIdleTimer(const net::TcpConnection::Ptr& conn) {
    const auto key = reinterpret_cast<uintptr_t>(conn.get());
    uint64_t tid = 0;
    {
        std::lock_guard<std::mutex> lk(idleMtx_);
        auto it = idleTimers_.find(key);
        if (it != idleTimers_.end()) {
            tid = it->second;
            idleTimers_.erase(it);
        }
    }
    if (tid != 0) loop_->cancel(tid);
}

HttpServer::~HttpServer() {
    stop();
}

core::Result<void> HttpServer::listen(const std::string& host, uint16_t port, int backlog) {
    return server_.listen(host, port, backlog);
}

core::Result<void> HttpServer::listenTls(const tls::OpenSslContext& ctx, const std::string& host,
                                         uint16_t port, int backlog) {
    server_.addConnectionDecorator([&ctx](const net::TcpConnection::Ptr& conn) {
        SSL* ssl = ctx.newServerSession();
        if (ssl == nullptr) {
            conn->forceClose();
            return;
        }
        conn->startTls(std::make_unique<tls::TlsFilter>(/*serverSide=*/true, ssl));
    });
    return server_.listen(host, port, backlog);
}

void HttpServer::stop() {
    server_.stop();
}

void HttpServer::beginGracefulShutdown(std::function<void()> onDrained) {
    closing_.store(true);
    drainedCb_ = std::move(onDrained);
    server_.stop();  // 停 accept；既有连接继续服务至排空
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

void HttpServer::enableWebSocket(const std::string& path,
                                 std::function<void(const net::WebSocketSession::Ptr&)> onOpen) {
    wsPaths_[path] = std::move(onOpen);
}

namespace {
/// h2 的 :method 是字符串，转回内部枚举（未知方法按 GET 处理并让路由决定 404/405）
HttpMethod methodFromH2(const std::string& m) {
    if (m == "GET") return HttpMethod::Get;
    if (m == "POST") return HttpMethod::Post;
    if (m == "PUT") return HttpMethod::Put;
    if (m == "DELETE") return HttpMethod::Delete;
    if (m == "HEAD") return HttpMethod::Head;
    if (m == "PATCH") return HttpMethod::Patch;
    if (m == "OPTIONS") return HttpMethod::Options;
    return HttpMethod::Get;
}
}  // namespace

void HttpServer::handleH2Request(const h2::H2Request& r, h2::H2Response& resp) {
    HttpRequest req;
    req.method = methodFromH2(r.method);
    req.httpMajor = 2;
    req.httpMinor = 0;
    req.peer = "h2";
    // :path 里带 query，拆成 path + query map（HTTP/1 解析器也是这么做的）
    const size_t q = r.path.find('?');
    if (q == std::string::npos) {
        req.path = r.path;
    } else {
        req.path = r.path.substr(0, q);
        req.rawQuery = r.path.substr(q + 1);
        for (const auto& [k, v] : parseQuery(req.rawQuery))
            req.query[k] = v;
    }
    for (const auto& [k, v] : r.headers)
        req.headers.add(k, v);
    req.body = r.body;

    Context ctx(std::move(req));
    ctx.setConnection(nullptr);  // h2 v1 暂不支持流式响应
    try {
        router_.dispatch(ctx);
    } catch (const Context::BadRequestBody& e) {
        ctx.status(400).text(400, std::string("400 Bad Request: ") + e.what());
    } catch (const std::exception& e) {
        ZLOG_ERROR("h2 handler exception: ", e.what(), " (", r.path, ")");
        ctx.status(500).text(500, "500 Internal Server Error");
    } catch (...) {
        ctx.status(500).text(500, "500 Internal Server Error");
    }

    resp.status = ctx.response().status;
    for (const auto& [k, v] : ctx.response().headers.entries())
        resp.headers.emplace_back(k, v);
    resp.body = ctx.response().body;
}

bool HttpServer::tryWebSocketUpgrade(const net::TcpConnection::Ptr& conn) {
    auto state = conn->context<ConnState>();
    const std::string path = state->parser.request().path;  // 先拷贝（reset 会清空内部请求）
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
    state->parser.reset();  // HTTP 阶段结束
    auto* self = this;
    loop_->queueInLoop([self, conn, path, earlyFrames]() mutable {
        auto session = net::WebSocketSession::takeover(conn, /*clientSide=*/false);
        auto it = self->wsPaths_.find(path);
        if (it != self->wsPaths_.end() && it->second) it->second(session);
        session->processPending(earlyFrames);  // 回放同包到达的早期帧
    });
    return true;
}

void HttpServer::onMessage(const net::TcpConnection::Ptr& conn, core::Buffer& buf) {
    auto state = conn->context<ConnState>();
    if (state == nullptr) {
        state = std::make_shared<ConnState>(maxBodyBytes_);
        conn->setContext(state);
    }

    // ---- M7.7：h2c（prior knowledge）----
    // 客户端第一个字节就是 "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"；一旦识别，整条连接交给 h2 层。
    if (state->h2 != nullptr) {
        const std::string out = state->h2->feed(
            std::string_view(reinterpret_cast<const char*>(buf.peek()), buf.readableBytes()));
        buf.consume(buf.readableBytes());
        if (!out.empty()) conn->send(out);
        if (state->h2->closed()) conn->shutdown();
        return;
    }
    if (buf.readableBytes() > 0) {
        const std::string_view pre = h2::kClientPreface;
        const size_t n = buf.readableBytes() < pre.size() ? buf.readableBytes() : pre.size();
        const std::string_view head(reinterpret_cast<const char*>(buf.peek()), n);
        if (head == pre.substr(0, n)) {
            if (n < pre.size()) return;  // preface 还没收全，等下一批
            state->h2 = std::make_unique<h2::H2ServerConnection>(
                [this](const h2::H2Request& req, h2::H2Response& resp) {
                    handleH2Request(req, resp);
                });
            const std::string out = state->h2->feed(
                std::string_view(reinterpret_cast<const char*>(buf.peek()), buf.readableBytes()));
            buf.consume(buf.readableBytes());
            if (!out.empty()) conn->send(out);
            return;
        }
    }

    for (;;) {
        const ParseOutcome out = state->parser.feed(buf);  // buf 字节全部转入解析器
        if (out.failed()) {
            HttpResponse resp;
            resp.status = 400;
            resp.headers.set("Content-Type", "text/plain; charset=utf-8");
            resp.body = "400 Bad Request: " + out.error;
            conn->send(resp.serialize(true, false));
            conn->shutdown();
            return;
        }
        if (!out.complete()) return;  // 等待更多数据

        handleRequest(conn);

        // pipelining：取出残留字节（可能是下一条请求的开头）
        std::string leftover = state->parser.releaseLeftover();
        state->parser.reset();
        if (leftover.empty()) return;
        buf.appendString(leftover);
    }
}

void HttpServer::handleRequest(const net::TcpConnection::Ptr& conn) {
    armIdleTimer(conn);  // 每请求续期空闲计时
    inflight_.fetch_add(1);
    auto inflightGuard = [this](void*) { maybeFireDrained(); };
    std::unique_ptr<void, decltype(inflightGuard)> guard{reinterpret_cast<void*>(this),
                                                         inflightGuard};

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

    // M7.2a：池化——首个请求构造，此后 reset 复用（params_/响应体等
    // 字符串容量保留在连接上，热路径省一串堆分配）
    if (state->ctx == nullptr) {
        state->ctx = std::make_unique<Context>(std::move(req));
    } else {
        state->ctx->reset(std::move(req));
    }
    Context& ctx = *state->ctx;
    ctx.response().httpMajor = ctx.request().httpMajor;
    ctx.response().httpMinor = ctx.request().httpMinor;
    ctx.setConnection(conn);  // M9.1：流式响应写端需要连接
    state->streaming = false;

    // M8.7：提取入站 W3C traceparent 并写入线程上下文（无则新起一条）。
    // 作用域覆盖整个派发过程：handler 里发起的下游 HttpClient 调用会自动
    // 带上同一条 trace，跨服务链路由此串起来——业务代码零改动。
    const obs::TraceScope traceScope(obs::TraceContext::parseOrGenerate(ctx.header("traceparent")));

    try {
        router_.dispatch(ctx);
    } catch (const Context::BadRequestBody& e) {
        ctx.status(400).text(400, std::string("400 Bad Request: ") + e.what());
    } catch (const std::exception& e) {
        ZLOG_ERROR("http handler exception: ", e.what(), " (", ctx.peer(), " ", ctx.request().path,
                   ")");
        ctx.status(500).text(500, std::string("500 Internal Server Error"));
    } catch (...) {
        ZLOG_ERROR("http handler unknown exception (", ctx.peer(), " ", ctx.request().path, ")");
        ctx.status(500).text(500, "500 Internal Server Error");
    }

    // 池化 Context 常驻连接：派发一结束就断开与连接的强引用。
    // 否则 连接→ConnState→Context→连接 构成循环，整条连接永不析构
    // （sse()/chunkedStream() 创建时各自持有连接强引用，这里断开安全）。
    ctx.setConnection(nullptr);

    const bool isHead = ctx.request().method == HttpMethod::Head;
    bool keep = ctx.request().keepAlive();
    if (closing_.load()) keep = false;  // 停机中：一律短连接

    // 流式响应（M9.1）：响应头已由 SseStream/ChunkWriter 写出，正文由 handler
    // 持有的写端继续推送；服务器此刻既不补 Content-Length，也不关连接。
    if (ctx.streaming()) {
        state->streaming = true;
        return;
    }
    // 只置了 chunked 却没开流（handler 误用）：补终止块，避免客户端一直等
    if (ctx.response().chunked) {
        std::string wire =
            ctx.response().serialize(/*includeBody=*/!isHead, /*connKeepAlive=*/false);
        if (!isHead) wire += "0\r\n\r\n";
        conn->send(wire);
        conn->shutdown();
        return;
    }

    // 未显式写响应体的合法情形：handler 未设置 body → 200 空体
    // M7.3：零拷贝正文——头部单独序列化，正文由 sendfile 直接从文件送出。
    // 无 sendfile 的平台/TLS 连接在 TcpConnection 内部回退为分块读发。
    if (ctx.hasFileBody() && !isHead) {
        const std::string path = ctx.fileBodyPath();
        const uint64_t off = ctx.fileBodyOffset();
        const uint64_t len = ctx.fileBodyLength();
        conn->send(ctx.response().serialize(/*includeBody=*/false, keep));
        conn->sendFile(path, off, len);
        if (!keep) conn->shutdown();
        return;
    }

    const std::string wire = ctx.response().serialize(/*includeBody=*/!isHead, keep);
    conn->send(wire);
    // 池化只留小缓冲：大响应体发完即还，避免空闲 keep-alive 连接长期占着
    // 上一次大响应的整块内存（M7.2a 的代价对冲）
    ctx.releaseLargeBuffers();
    if (!keep) conn->shutdown();
}

}  // namespace zenith::http
