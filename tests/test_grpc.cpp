// gRPC 传输层：消息帧 + 跑在 HTTP/2 上的端到端调用
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ztest.hpp"

#include "zenith/http/grpc.hpp"
#include "zenith/http/http2.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/event_loop_thread_pool.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
class Raw {
public:
    ~Raw() { close(); }
    bool open(uint16_t port) {
#if defined(_WIN32)
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_ < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    }
    bool sendAll(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            const int n = ::send(fd_, s.data() + off, static_cast<int>(s.size() - off), 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }
    std::string readFor(int timeoutMs) {
        std::string out;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0) break;
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(fd_, &rd);
            timeval tv{};
            tv.tv_sec = static_cast<long>(left / 1000);
            tv.tv_usec = static_cast<long>((left % 1000) * 1000);
            if (::select(static_cast<int>(fd_ + 1), &rd, nullptr, nullptr, &tv) <= 0) break;
            char buf[65536];
            const int n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
            if (out.size() > 4096) break;
        }
        return out;
    }
    void close() {
        if (fd_ < 0) return;
#if defined(_WIN32)
        closesocket(fd_);
#else
        ::close(fd_);
#endif
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

/// 一次 gRPC 调用的回包拆解结果
struct GrpcReply {
    bool gotHeaders = false;
    bool gotTrailers = false;
    std::string body;
    std::string grpcStatus;
    std::string grpcMessage;
    std::string contentType;
};

GrpcReply parseReply(const std::string& wire) {
    GrpcReply out;
    http::h2::HpackDecoder dec;
    size_t pos = 0;
    while (pos < wire.size()) {
        auto r = http::h2::parseFrame(std::string_view(wire).substr(pos));
        if (r.status != http::h2::ParseResult::Status::Ok) break;
        pos += r.consumed;
        if (r.frame.streamId != 1) continue;
        if (r.frame.type == http::h2::FrameType::Data) {
            out.body += r.frame.payload;
        } else if (r.frame.type == http::h2::FrameType::Headers) {
            std::vector<std::pair<std::string, std::string>> fields;
            if (!dec.decode(r.frame.payload, fields)) continue;
            const bool endStream = (r.frame.flags & 0x1) != 0;
            for (const auto& [k, v] : fields) {
                if (k == "content-type") out.contentType = v;
                if (k == "grpc-status") out.grpcStatus = v;
                if (k == "grpc-message") out.grpcMessage = v;
            }
            // 没有 END_STREAM 的 HEADERS 是响应头；带 END_STREAM 的是尾随头
            if (endStream) {
                out.gotTrailers = true;
            } else {
                out.gotHeaders = true;
            }
        }
    }
    return out;
}
}  // namespace

/* ------------------------------ 消息帧 ------------------------------ */

ZTEST(Grpc, MessageFramingRoundtrip) {
    const std::string payload = "hello, protobuf bytes";
    const std::string wire = http::grpc::encodeMessage(payload);
    ZEXPECT_EQ(wire.size(), payload.size() + 5);

    auto r = http::grpc::decodeMessage(wire);
    ZEXPECT_TRUE(r.status == http::grpc::MessageResult::Status::Ok);
    ZEXPECT_EQ(r.payload, payload);
    ZEXPECT_EQ(r.consumed, wire.size());
}

ZTEST(Grpc, MessageFramingPendingAndErrors) {
    const std::string wire = http::grpc::encodeMessage("abcdef");
    ZEXPECT_TRUE(http::grpc::decodeMessage(wire.substr(0, 3)).status ==
                 http::grpc::MessageResult::Status::Pending);
    ZEXPECT_TRUE(http::grpc::decodeMessage(wire.substr(0, wire.size() - 1)).status ==
                 http::grpc::MessageResult::Status::Pending);

    std::string compressed = wire;
    compressed[0] = 1;  // 声明压缩但不支持
    auto e = http::grpc::decodeMessage(compressed);
    ZEXPECT_TRUE(e.status == http::grpc::MessageResult::Status::Error);
    ZEXPECT_TRUE(e.error.find("compressed") != std::string::npos);
}

ZTEST(Grpc, ContentTypeDetection) {
    ZEXPECT_TRUE(http::grpc::isGrpcContentType("application/grpc"));
    ZEXPECT_TRUE(http::grpc::isGrpcContentType("application/grpc+proto"));
    ZEXPECT_FALSE(http::grpc::isGrpcContentType("application/json"));
}

/* ------------------------------- 端到端 ------------------------------- */

ZTEST(GrpcE2E, UnaryCallOverH2c) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);

    http::grpc::Service svc;
    svc.method("/helloworld.Greeter/SayHello",
               [](const http::grpc::Call& call, std::string& reply) {
                   reply = "hello, " + call.message;
                   return http::grpc::Code::Ok;
               });
    svc.mount(server);
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&loop] { loop.loop(); });
    std::this_thread::sleep_for(150ms);
    struct Cleanup {
        net::EventLoop& loop;
        std::thread& t;
        net::EventLoopThreadPool& io;
        ~Cleanup() {
            loop.stop();
            if (t.joinable()) t.join();
            io.stop();
        }
    } cleanup{loop, t, io};

    Raw raw;
    ZEXPECT_TRUE(raw.open(server.port()));

    std::string req(http::h2::kClientPreface);
    http::h2::Frame settings;
    settings.type = http::h2::FrameType::Settings;
    req += http::h2::encodeFrame(settings);

    http::h2::HpackEncoder enc;
    http::h2::Frame headers;
    headers.type = http::h2::FrameType::Headers;
    headers.streamId = 1;
    headers.flags = 0x4;  // END_HEADERS（正文随后用 DATA 送）
    headers.payload = enc.encode({{":method", "POST"},
                                  {":scheme", "http"},
                                  {":path", "/helloworld.Greeter/SayHello"},
                                  {":authority", "127.0.0.1"},
                                  {"content-type", "application/grpc"},
                                  {"te", "trailers"}});
    req += http::h2::encodeFrame(headers);

    http::h2::Frame data;
    data.type = http::h2::FrameType::Data;
    data.streamId = 1;
    data.flags = 0x1;  // END_STREAM
    data.payload = http::grpc::encodeMessage("zenith");
    req += http::h2::encodeFrame(data);

    ZEXPECT_TRUE(raw.sendAll(req));
    const std::string wire = raw.readFor(3000);
    ZEXPECT_TRUE(!wire.empty());

    const GrpcReply reply = parseReply(wire);
    ZEXPECT_TRUE(reply.gotHeaders);
    ZEXPECT_TRUE(reply.gotTrailers);
    ZEXPECT_EQ(reply.contentType, std::string("application/grpc"));
    ZEXPECT_EQ(reply.grpcStatus, std::string("0"));

    auto msg = http::grpc::decodeMessage(reply.body);
    ZEXPECT_TRUE(msg.status == http::grpc::MessageResult::Status::Ok);
    ZEXPECT_EQ(msg.payload, std::string("hello, zenith"));
    raw.close();
}

ZTEST(GrpcE2E, UnknownMethodReturnsUnimplemented) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    http::grpc::Service svc;
    svc.method("/svc/Exists", [](const http::grpc::Call&, std::string& reply) {
        reply = "ok";
        return http::grpc::Code::Ok;
    });
    svc.mount(server);
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&loop] { loop.loop(); });
    std::this_thread::sleep_for(150ms);
    struct Cleanup {
        net::EventLoop& loop;
        std::thread& t;
        net::EventLoopThreadPool& io;
        ~Cleanup() {
            loop.stop();
            if (t.joinable()) t.join();
            io.stop();
        }
    } cleanup{loop, t, io};

    Raw raw;
    ZEXPECT_TRUE(raw.open(server.port()));
    std::string req(http::h2::kClientPreface);
    http::h2::Frame settings;
    settings.type = http::h2::FrameType::Settings;
    req += http::h2::encodeFrame(settings);
    http::h2::HpackEncoder enc;
    http::h2::Frame headers;
    headers.type = http::h2::FrameType::Headers;
    headers.streamId = 1;
    headers.flags = 0x4 | 0x1;  // END_HEADERS | END_STREAM（无正文）
    headers.payload = enc.encode({{":method", "POST"},
                                  {":scheme", "http"},
                                  {":path", "/svc/Missing"},
                                  {":authority", "127.0.0.1"},
                                  {"content-type", "application/grpc"}});
    req += http::h2::encodeFrame(headers);
    ZEXPECT_TRUE(raw.sendAll(req));

    const GrpcReply reply = parseReply(raw.readFor(3000));
    ZEXPECT_TRUE(reply.gotTrailers);
    ZEXPECT_EQ(reply.grpcStatus, std::string("12"));  // UNIMPLEMENTED
    raw.close();
}

ZTEST_MAIN
