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
    if (req.port != 80 && req.port != 443 && req.port != 0) {
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
    out += "\r\nConnection: ";
    out += req.keepAlive ? "keep-alive" : "close";
    out += "\r\n\r\n";
    out += req.body;
    return out;
}

/// URL 解析并回填请求字段
core::Result<void> applyUrl(ClientRequest& req) {
    if (req.url.empty()) return core::Result<void>::Ok();
    auto u = parseUrl(req.url);
    if (!u) return core::Result<void>::Err(std::move(u).error());
    req.host = u.value().host;
    req.port = u.value().port;
    req.path = u.value().pathAndQuery;
    req.tls = u.value().tls;
    if (req.port == 0) req.port = req.tls ? 443 : 80;
    return core::Result<void>::Ok();
}

std::string poolKeyOf(const ClientRequest& req) {
    return (req.tls ? "T:" : "") + req.host + ":" + std::to_string(req.port);
}

} // namespace

void HttpClient::finishState(const std::shared_ptr<AsyncState>& st,
                              core::Result<HttpResponse> r) {
    if (st->done) return;
    st->done = true;
    if (st->timerId != 0 && st->loop != nullptr) st->loop->cancel(st->timerId);

    // keep-alive 成功响应 → 解除回调、归还连接池；否则关闭。
    // 注意：正在消息回调内执行—— setMessageCallback 会销毁持有 st 的当前闭包，
    // 同步继续访问即 UAF（M3 5d 规则）→ 解除动作投递到回调返回之后。
    const bool reuse = r && r.value().keepAlive() && st->reusable && st->conn &&
                       st->conn->connected();
    if (st->conn) {
        if (reuse) {
            st->loop->queueInLoop([this, st] {
                if (st->conn) {
                    st->conn->setMessageCallback(nullptr);
                    st->conn->setCloseCallback(nullptr);
                    returnToPool(st);
                }
            });
        } else {
            st->conn->forceClose();
        }
    }
    if (st->cb) {
        auto cb = std::move(st->cb);
        cb(std::move(r));
    }
}

void HttpClient::returnToPool(const std::shared_ptr<AsyncState>& st) {
    if (!st->conn) return;
    std::lock_guard<std::mutex> lk(poolMtx_);
    auto& vec = pool_[st->poolKey];
    if (vec.size() < kMaxPoolPerHost) {
        vec.push_back(st->conn);
    } else {
        st->conn->forceClose();
    }
}

std::shared_ptr<tls::OpenSslContext> HttpClient::tlsContextFor(const ClientRequest& req) {
    if (tlsCtx_ && tlsCtx_->valid()) return tlsCtx_;
    tls::TlsClientConfig cfg;
    cfg.verifyPeer = req.verifyPeer;
    cfg.caFile = req.caFile;
    auto ctx = tls::OpenSslContext::createClient(cfg);
    if (!ctx) return nullptr;
    tlsCtx_ = std::make_shared<tls::OpenSslContext>(std::move(ctx).value());
    return tlsCtx_;
}

HttpClient::HttpClient(net::EventLoop* loop) : loop_(loop) {}

void HttpClient::clearPool() {
    std::lock_guard<std::mutex> lk(poolMtx_);
    for (auto& [k, vec] : pool_) {
        for (auto& c : vec) c->forceClose();
    }
    pool_.clear();
}

size_t HttpClient::pooledConnections() const {
    std::lock_guard<std::mutex> lk(poolMtx_);
    size_t n = 0;
    for (const auto& [k, vec] : pool_) n += vec.size();
    return n;
}

void HttpClient::issueOn(const net::TcpConnection::Ptr& conn, const ClientRequest& req,
                          const std::shared_ptr<AsyncState>& st) {
    st->conn = conn;
    st->reusable = req.keepAlive;
    st->timerId = st->loop->scheduleAfter(std::chrono::milliseconds(req.timeoutMs),
                                          [this, st] {
                                              finishState(st, core::Result<HttpResponse>::Err(
                                                                   core::Error(30,
                                                                               "http response timeout")));
                                          });
    conn->setMessageCallback([this, st](const net::TcpConnection::Ptr&, core::Buffer& buf) {
        const ParseOutcome out = st->parser.feed(buf);
        if (out.failed()) {
            finishState(st, core::Result<HttpResponse>::Err(
                                 core::Error(31, "bad response: " + out.error)));
            return;
        }
        if (out.complete()) {
            finishState(st, core::Result<HttpResponse>::Ok(st->parser.response()));
        }
    });
    conn->setCloseCallback([this, st](const net::TcpConnection::Ptr&) {
        if (st->done) {
            // 已完成（可能已归还池）——从池中惰性剔除由复用检查负责
            return;
        }
        if (st->parser.hasParsed()) {
            // 无 Content-Length 的响应：对端关闭即完成
            finishState(st, core::Result<HttpResponse>::Ok(st->parser.response()));
        } else {
            finishState(st, core::Result<HttpResponse>::Err(
                                 core::Error(32, "connection closed before response")));
        }
    });
    conn->send(serializeRequest(req));
}

void HttpClient::request(const ClientRequest& reqIn, ResponseCallback cb) {
    ClientRequest req = reqIn;
    if (!applyUrl(req)) {
        if (cb) cb(core::Result<HttpResponse>::Err(core::Error(93, "bad url: " + reqIn.url)));
        return;
    }

    auto st = std::make_shared<AsyncState>();
    st->cb = std::move(cb);
    st->loop = loop_;
    st->poolKey = poolKeyOf(req);

    // 1) 连接池复用
    {
        net::TcpConnection::Ptr pooled;
        {
            std::lock_guard<std::mutex> lk(poolMtx_);
            auto& vec = pool_[st->poolKey];
            while (!vec.empty()) {
                auto cand = vec.back();
                vec.pop_back();
                if (cand->connected() && (!req.tls || cand->tlsEstablished())) {
                    pooled = cand;
                    break;
                }
                // 失效连接丢弃
            }
        }
        if (pooled) {
            // 跨线程时投递到 loop 线程再操作连接
            auto* self = this;
            loop_->runInLoop([self, pooledSt = st, conn = pooled, req]() {
                self->issueOn(conn, req, pooledSt);
            });
            return;
        }
    }

    // 2) 新建连接
    auto client = std::make_shared<net::TcpClient>(loop_);
    net::ConnectOptions copt;
    copt.host = req.host;
    copt.port = req.port;
    copt.timeoutMs = req.timeoutMs;

    const std::string wire = serializeRequest(req);

    client->connect(
        copt,
        [this, st, req](const net::TcpConnection::Ptr& conn) {
            // HTTPS：先装 TLS 滤层（客户端角色 + SNI），请求明文会在握手后自动发出
            if (req.tls) {
                auto ctx = tlsContextFor(req);
                SSL* ssl = ctx != nullptr ? ctx->newClientSession(req.host) : nullptr;
                if (ssl == nullptr) {
                    finishState(st, core::Result<HttpResponse>::Err(
                                         core::Error(94, "tls setup failed")));
                    conn->forceClose();
                    return;
                }
                conn->startTls(std::make_unique<tls::TlsFilter>(false, ssl));
            }
            issueOn(conn, req, st);
        },
        [this, st](const std::string& reason) {
            finishState(st,
                        core::Result<HttpResponse>::Err(core::Error(33, "connect failed: " + reason)));
        });
    // client：成功/失败回调由 TcpClient 内部 shared_from_this 保活至判定完成
}

core::Result<HttpResponse> HttpClient::fetchBlocking(const ClientRequest& req) {
    net::EventLoop loop; // 在调用线程上运行的临时 loop

    core::Result<HttpResponse> result =
        core::Result<HttpResponse>::Err(core::Error(40, "not started"));
    std::atomic<bool> got{false};

    {
        HttpClient client(&loop);
        loop.queueInLoop([&loop, &client, &req, &result, &got] {
            client.request(req, [&loop, &result, &got](core::Result<HttpResponse> r) {
                result = std::move(r);
                got.store(true, std::memory_order_release);
                loop.stop();
            });
        });

        loop.loop();
    }
    if (!got.load(std::memory_order_acquire)) {
        return core::Result<HttpResponse>::Err(core::Error(41, "internal: no result"));
    }
    return result;
}

core::Result<HttpResponse> HttpClient::fetchBlockingUrl(const std::string& url,
                                                          HttpMethod m, std::string body) {
    ClientRequest rq;
    rq.url = url;
    rq.method = m;
    rq.body = std::move(body);
    rq.timeoutMs = 10000;
    return fetchBlocking(rq);
}

} // namespace zenith::http
