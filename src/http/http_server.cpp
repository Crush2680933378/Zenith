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
    : server_(loop, ioPool), maxBodyBytes_(maxBody) {
    server_.setMessageCallback(
        [this](const net::TcpConnection::Ptr& conn, core::Buffer& buf) {
            onMessage(conn, buf);
        });
}

HttpServer::~HttpServer() { stop(); }

core::Result<void> HttpServer::listen(const std::string& host, uint16_t port, int backlog) {
    return server_.listen(host, port, backlog);
}

void HttpServer::stop() { server_.stop(); }

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
    auto state = conn->context<ConnState>();
    HttpRequest req = state->parser.request();
    req.peer = conn->peer();

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
    const bool keep = ctx.request().keepAlive();
    const std::string wire = ctx.response().serialize(/*includeBody=*/!isHead, keep);
    conn->send(wire);
    if (!keep) conn->shutdown();
}

} // namespace zenith::http
