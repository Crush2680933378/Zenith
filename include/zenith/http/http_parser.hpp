// ============================================================================
//  zenith::http 解析器 —— 增量式 HTTP/1.1（请求 & 响应）
//  - 支持请求行/头部/Content-Length 与 chunked 主体
//  - 字节流可任意切分喂入（1 字节也能正确解析）
//  - 限制：请求行 8KB / 头部 64KB / 主体默认 10MB（可配）
// ============================================================================
#pragma once

#include <string>

#include "zenith/core/buffer.hpp"
#include "zenith/core/export.hpp"
#include "zenith/http/http_message.hpp"

namespace zenith::http {

struct ParseOutcome {
    enum class State { Incomplete, Complete, Error };
    State state = State::Incomplete;
    std::string error;
    bool complete() const noexcept { return state == State::Complete; }
    bool failed() const noexcept { return state == State::Error; }
};

class ZENITH_API HttpRequestParser {
public:
    explicit HttpRequestParser(size_t maxBodyBytes = 10 * 1024 * 1024);

    /// 喂入数据；完成后 request() 有效；再次解析下一条前 reset()。
    ParseOutcome feed(const char* data, size_t n);

    /// 便捷：从 Buffer 取（等价 feed(peek, readable) 后 consume）。
    ParseOutcome feed(core::Buffer& buf);

    [[nodiscard]] const HttpRequest& request() const noexcept { return req_; }
    [[nodiscard]] size_t maxBodyBytes() const noexcept { return maxBody_; }
    void setMaxBodyBytes(size_t n) noexcept { maxBody_ = n; }
    void reset();

    /// 取出未消费的残留字节（pipelining：下一条请求的开头）。不移除已完成状态。
    std::string releaseLeftover() {
        std::string out;
        out.swap(pending_);
        return out;
    }

private:
    ParseOutcome tryParse();
    ParseOutcome fail(std::string msg);
    bool parseRequestLine(const std::string& line);
    bool parseHeaderLine(const std::string& line);

    enum class State : uint8_t {
        ReqLine,
        Headers,
        BodyIdentity,   // Content-Length
        BodyChunkSize,
        BodyChunkData,
        BodyChunkCrlf,  // 数据块后的 CRLF
        BodyChunkTrail, // 0 大小块后的 trailer 头
        Done,
    };

    HttpRequest req_;
    std::string pending_;
    State state_ = State::ReqLine;
    size_t bodyRemaining_ = 0;
    size_t chunkRemaining_ = 0;
    size_t maxBody_;
};

/// 客户端侧响应解析（状态行 + 头 + CL/chunked 主体）。
class ZENITH_API HttpResponseParser {
public:
    HttpResponseParser();

    ParseOutcome feed(const char* data, size_t n);
    ParseOutcome feed(core::Buffer& buf);

    [[nodiscard]] const HttpResponse& response() const noexcept { return resp_; }
    void reset();

private:
    ParseOutcome tryParse();
    ParseOutcome fail(std::string msg);

    enum class State : uint8_t {
        StatusLine,
        Headers,
        BodyIdentity,
        BodyChunkSize,
        BodyChunkData,
        BodyChunkCrlf,
        BodyChunkTrail,
        ReadUntilClose, // 无 CL 且非 chunked：读到对端关闭
        Done,
    };

    HttpResponse resp_;
    std::string pending_;
    State state_ = State::StatusLine;
    size_t bodyRemaining_ = 0;
    size_t chunkRemaining_ = 0;
};

} // namespace zenith::http
