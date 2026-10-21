// ============================================================================
//  zenith::http::ChunkWriter / SseStream —— HTTP/1.1 分块流式响应（M9.1）
//  - ChunkWriter：先写响应头（Transfer-Encoding: chunked），再逐块写数据
//  - SseStream  ：text/event-stream 语义封装（event/id/data + 心跳注释）
//  两者线程安全（TcpConnection::send 跨线程自动投递），写端持有连接保活。
//  生命周期：start() 之后必须 close()（或对端断开）——流式连接不复用 keep-alive。
//  定位：ai_inference 的 token 流、ai_mcp 的 Streamable HTTP 响应流、
//  A2A 的 message/stream 都以这里为唯一出口。
// ============================================================================
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/net/tcp_connection.hpp"

namespace zenith::http {

struct HttpResponse;

/// 分块写端：start() 发出响应头，此后只写分块正文。
class ZENITH_API ChunkWriter : public std::enable_shared_from_this<ChunkWriter> {
public:
    using Ptr = std::shared_ptr<ChunkWriter>;

    /// 发送响应头（head.chunked 会强制置位）并返回写端；conn 为空返回 nullptr。
    static Ptr start(const net::TcpConnection::Ptr& conn, const HttpResponse& head);

    /// 写一块数据（空数据忽略）。
    void write(std::string_view data);
    /// 写终止块 0\r\n\r\n 并关连接；可重复调用。
    void close();

    [[nodiscard]] bool closed() const noexcept { return closed_.load(); }
    /// 对端是否仍在线（断线后写入自动静默丢弃）。
    [[nodiscard]] bool alive() const noexcept;

private:
    explicit ChunkWriter(net::TcpConnection::Ptr conn) : conn_(std::move(conn)) {}

    net::TcpConnection::Ptr conn_; // 流存活期间保活连接
    std::atomic<bool> closed_{false};
};

/// SSE 流：把事件编码成 text/event-stream 帧后交给 ChunkWriter。
class ZENITH_API SseStream : public std::enable_shared_from_this<SseStream> {
public:
    using Ptr = std::shared_ptr<SseStream>;
    using Headers = std::vector<std::pair<std::string, std::string>>;

    /// 发送事件流响应头（Content-Type/Cache-Control/X-Accel-Buffering 已内置）。
    static Ptr start(const net::TcpConnection::Ptr& conn, int status = 200,
                     const Headers& extraHeaders = {});

    /// 发一条事件。data 含换行时自动拆成多行 `data:`（WHATWG EventSource 语义）。
    void send(std::string_view data, std::string_view event = {}, std::string_view id = {});
    /// 建议客户端重连间隔（retry: 毫秒）。
    void retry(int milliseconds);
    /// 注释行（心跳保活，浏览器忽略）：": text"。
    void comment(std::string_view text = {});
    /// 结束流：写终止块并关连接。
    void close();

    [[nodiscard]] bool closed() const noexcept;

private:
    explicit SseStream(ChunkWriter::Ptr w) : w_(std::move(w)) {}

    ChunkWriter::Ptr w_;
};

} // namespace zenith::http
