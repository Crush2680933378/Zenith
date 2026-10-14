#include "zenith/http/http_client.hpp"

#include "zenith/net/tcp_client.hpp"

namespace zenith::http {

namespace {

std::string serializeRequest(const ClientRequest& req) {
    std::string out;
    out.reserve(128 + req.headers.size() * 32 + req.body.size());
    out += toString(req.method);
    out += ' ';
    out += req.path;
    out += " HTTP/1.1\r\nHost: ";
    out += req.host;
    if (req.port != 80 && req.port != 0) {
        out += ':';
        out += std::to_string(req.port);
    }
    out += "\r\n";
    for (const auto& [k, v] : req.headers.entries()) {
        if (iequals(k, "Host") || iequals(k, "Content-Length") || iequals(k, "Connection")) {
            continue; // 由本函数统一控制
        }
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(req.body.size());
    out += "\r\nConnection: close\r\n\r\n";
    out += req.body;
    return out;
}

} // namespace

void HttpClient::finishState(const std::shared_ptr<AsyncState>& st,
                              core::Result<HttpResponse> r) {
    if (st->done) return;
    st->done = true;
    if (st->timerId != 0 && st->loop != nullptr) st->loop->cancel(st->timerId);
    if (st->conn) st->conn->forceClose();
    if (st->cb) {
        auto cb = std::move(st->cb);
        cb(std::move(r));
    }
}

HttpClient::HttpClient(net::EventLoop* loop) : loop_(loop) {}

void HttpClient::request(const ClientRequest& req, ResponseCallback cb) {
    auto st = std::make_shared<AsyncState>();
    st->cb = std::move(cb);
    st->loop = loop_;

    auto client = std::make_shared<net::TcpClient>(loop_);
    net::ConnectOptions copt;
    copt.host = req.host;
    copt.port = req.port;
    copt.timeoutMs = req.timeoutMs;

    const int timeoutMs = req.timeoutMs;
    const std::string wire = serializeRequest(req);

    client->connect(
        copt,
        [st, timeoutMs, wire](const net::TcpConnection::Ptr& conn) {
            st->conn = conn;
            st->timerId = st->loop->scheduleAfter(std::chrono::milliseconds(timeoutMs), [st] {
                finishState(st, core::Result<HttpResponse>::Err(
                                    core::Error(30, "http response timeout")));
            });
            conn->setMessageCallback(
                [st](const net::TcpConnection::Ptr&, core::Buffer& buf) {
                    const ParseOutcome out = st->parser.feed(buf);
                    if (out.failed()) {
                        finishState(st, core::Result<HttpResponse>::Err(
                                             core::Error(31, "bad response: " + out.error)));
                        return;
                    }
                    if (out.complete()) {
                        finishState(st, core::Result<HttpResponse>::Ok(
                                             st->parser.response()));
                    }
                });
            conn->setCloseCallback([st](const net::TcpConnection::Ptr&) {
                if (st->done) return;
                if (st->parser.response().status != 0) {
                    // 无 Content-Length 的响应：对端关闭即完成
                    finishState(st, core::Result<HttpResponse>::Ok(st->parser.response()));
                } else {
                    finishState(st, core::Result<HttpResponse>::Err(
                                         core::Error(32, "connection closed before response")));
                }
            });
            conn->send(wire);
        },
        [st](const std::string& reason) {
            finishState(st, core::Result<HttpResponse>::Err(
                                 core::Error(33, "connect failed: " + reason)));
        });
    // client：成功/失败回调由 TcpClient 内部 shared_from_this 保活至判定完成
}

core::Result<HttpResponse> HttpClient::fetchBlocking(const ClientRequest& req) {
    net::EventLoop loop; // 在调用线程上运行的临时 loop

    core::Result<HttpResponse> result =
        core::Result<HttpResponse>::Err(core::Error(40, "not started"));
    std::atomic<bool> got{false};

    HttpClient client(&loop);
    loop.queueInLoop([&loop, &client, &req, &result, &got] {
        client.request(req, [&loop, &result, &got](core::Result<HttpResponse> r) {
            result = std::move(r);
            got.store(true, std::memory_order_release);
            loop.stop();
        });
    });

    loop.loop();
    if (!got.load(std::memory_order_acquire)) {
        return core::Result<HttpResponse>::Err(core::Error(41, "internal: no result"));
    }
    return result;
}

} // namespace zenith::http
