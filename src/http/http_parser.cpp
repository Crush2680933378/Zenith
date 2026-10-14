#include "zenith/http/http_parser.hpp"

#include <cstdlib>

namespace zenith::http {

namespace {

constexpr size_t kMaxRequestLine = 8 * 1024;
constexpr size_t kMaxHeaderBytes = 64 * 1024;

/// 从 pending 中取一行（不含 CRLF）。找到返回 true 并消费；
/// 溢出限制返回 -1（错误）；数据不足返回 0。
int takeLine(std::string& pending, std::string& line, size_t limit) {
    const size_t nl = pending.find("\r\n");
    if (nl == std::string::npos) {
        return pending.size() > limit ? -1 : 0;
    }
    if (nl > limit) return -1;
    line.assign(pending, 0, nl);
    pending.erase(0, nl + 2);
    return 1;
}

/// 解析 chunk 大小行（十六进制，可带扩展）。
bool parseChunkSize(const std::string& line, size_t& out) {
    size_t end = line.find(';');
    if (end == std::string::npos) end = line.size();
    const std::string hex = line.substr(0, end);
    if (hex.empty() || hex.size() > 16) return false;
    size_t v = 0;
    for (char c : hex) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + static_cast<size_t>(d);
        if (v > (1ull << 40)) return false;
    }
    out = v;
    return true;
}

} // namespace

/* ==========================================================================
 * HttpRequestParser
 * ======================================================================== */

HttpRequestParser::HttpRequestParser(size_t maxBody) : maxBody_(maxBody) {}

ParseOutcome HttpRequestParser::feed(const char* data, size_t n) {
    if (state_ == State::Done) {
        return fail("parser already completed (reset before next request)");
    }
    pending_.append(data, n);
    return tryParse();
}

ParseOutcome HttpRequestParser::feed(core::Buffer& buf) {
    if (buf.readableBytes() == 0) return {};
    const ParseOutcome out = feed(reinterpret_cast<const char*>(buf.peek()),
                                   buf.readableBytes());
    buf.consume(buf.readableBytes()); // 解析器已拷贝所需字节
    return out;
}

void HttpRequestParser::reset() {
    req_ = HttpRequest{};
    pending_.clear();
    state_ = State::ReqLine;
    bodyRemaining_ = 0;
    chunkRemaining_ = 0;
}

ParseOutcome HttpRequestParser::fail(std::string msg) {
    state_ = State::Done;
    ParseOutcome o;
    o.state = ParseOutcome::State::Error;
    o.error = std::move(msg);
    return o;
}

ParseOutcome HttpRequestParser::tryParse() {
    for (;;) {
        switch (state_) {
            case State::ReqLine: {
                std::string line;
                const int r = takeLine(pending_, line, kMaxRequestLine);
                if (r <= 0) return r < 0 ? fail("request line too long") : ParseOutcome{};
                if (line.empty()) continue; // 跳过请求行前的空行（容忍）
                if (!parseRequestLine(line)) return fail("malformed request line");
                state_ = State::Headers;
                continue;
            }
            case State::Headers: {
                std::string line;
                const int r = takeLine(pending_, line, kMaxHeaderBytes);
                if (r <= 0) return r < 0 ? fail("headers too large") : ParseOutcome{};
                if (line.empty()) {
                    // 头部结束：确定主体形态
                    const bool chunked = req_.headers.contains("Transfer-Encoding") &&
                        core::string_util::toLower(req_.headers.get("Transfer-Encoding"))
                            .find("chunked") != std::string::npos;
                    if (chunked) {
                        state_ = State::BodyChunkSize;
                    } else {
                        const size_t cl = req_.contentLength();
                        if (cl > maxBody_) return fail("body too large");
                        bodyRemaining_ = cl;
                        state_ = cl > 0 ? State::BodyIdentity : State::Done;
                    }
                    continue;
                }
                if (!parseHeaderLine(line)) return fail("malformed header line");
                continue;
            }
            case State::BodyIdentity: {
                const size_t avail = pending_.size();
                const size_t take = avail < bodyRemaining_ ? avail : bodyRemaining_;
                if (take > 0) {
                    req_.body.append(pending_, 0, take);
                    pending_.erase(0, take);
                    bodyRemaining_ -= take;
                }
                if (bodyRemaining_ == 0) state_ = State::Done;
                else return ParseOutcome{};
                continue;
            }
            case State::BodyChunkSize: {
                std::string line;
                const int r = takeLine(pending_, line, 64);
                if (r <= 0) return r < 0 ? fail("bad chunk size line") : ParseOutcome{};
                if (!parseChunkSize(line, chunkRemaining_)) return fail("bad chunk size");
                state_ = chunkRemaining_ == 0 ? State::BodyChunkTrail : State::BodyChunkData;
                continue;
            }
            case State::BodyChunkData: {
                const size_t avail = pending_.size();
                const size_t take = avail < chunkRemaining_ ? avail : chunkRemaining_;
                if (take > 0) {
                    if (req_.body.size() + take > maxBody_) return fail("body too large");
                    req_.body.append(pending_, 0, take);
                    pending_.erase(0, take);
                    chunkRemaining_ -= take;
                }
                if (chunkRemaining_ == 0) state_ = State::BodyChunkCrlf;
                else return ParseOutcome{};
                continue;
            }
            case State::BodyChunkCrlf: {
                // 每个数据块结束后必须有且仅有一个空行（CRLF）
                std::string line;
                const int r = takeLine(pending_, line, 0);
                if (r <= 0) return r < 0 ? fail("bad chunk framing") : ParseOutcome{};
                if (!line.empty()) return fail("bad chunk framing (expected CRLF)");
                state_ = State::BodyChunkSize;
                continue;
            }
            case State::BodyChunkTrail: {
                // 0 大小块之后的 trailer 头（M2 忽略其内容）直至空行
                std::string line;
                const int r = takeLine(pending_, line, kMaxHeaderBytes);
                if (r <= 0) return r < 0 ? fail("bad chunk trailer") : ParseOutcome{};
                if (line.empty()) state_ = State::Done;
                continue;
            }
            case State::Done: {
                ParseOutcome o;
                o.state = ParseOutcome::State::Complete;
                // query 解析（含 percent 解码）
                if (!req_.rawQuery.empty()) {
                    for (auto& [k, v] : parseQuery(req_.rawQuery)) {
                        req_.query[k] = std::move(v);
                    }
                }
                return o;
            }
        }
    }
}

bool HttpRequestParser::parseRequestLine(const std::string& line) {
    // METHOD SP TARGET SP VERSION
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    req_.method = methodFromString(line.substr(0, sp1));
    if (req_.method == HttpMethod::Unknown) return false;

    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string version = line.substr(sp2 + 1);
    if (version == "HTTP/1.1") {
        req_.httpMajor = 1; req_.httpMinor = 1;
    } else if (version == "HTTP/1.0") {
        req_.httpMajor = 1; req_.httpMinor = 0;
    } else {
        return false;
    }

    const size_t q = target.find('?');
    if (q == std::string::npos) {
        req_.path = urlDecode(target, false);
        req_.rawQuery.clear();
    } else {
        req_.path = urlDecode(target.substr(0, q), false);
        req_.rawQuery = target.substr(q + 1);
    }
    if (req_.path.empty() || req_.path[0] != '/') return false;
    return true;
}

bool HttpRequestParser::parseHeaderLine(const std::string& line) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    const std::string name = core::string_util::trim(line.substr(0, colon));
    const std::string value = core::string_util::trim(line.substr(colon + 1));
    if (name.empty()) return false;
    for (char c : name) {
        if (c == ' ' || c == '\t') return false; // 头名不允许空白
    }
    req_.headers.add(name, value);
    return true;
}

/* ==========================================================================
 * HttpResponseParser
 * ======================================================================== */

HttpResponseParser::HttpResponseParser() = default;

ParseOutcome HttpResponseParser::feed(const char* data, size_t n) {
    if (state_ == State::Done) {
        ParseOutcome o;
        o.state = ParseOutcome::State::Error;
        o.error = "parser already completed";
        return o;
    }
    pending_.append(data, n);
    return tryParse();
}

ParseOutcome HttpResponseParser::feed(core::Buffer& buf) {
    if (buf.readableBytes() == 0) return {};
    const ParseOutcome out = feed(reinterpret_cast<const char*>(buf.peek()),
                                   buf.readableBytes());
    buf.consume(buf.readableBytes());
    return out;
}

void HttpResponseParser::reset() {
    resp_ = HttpResponse{};
    pending_.clear();
    state_ = State::StatusLine;
    bodyRemaining_ = 0;
    chunkRemaining_ = 0;
}

ParseOutcome HttpResponseParser::fail(std::string msg) {
    state_ = State::Done;
    ParseOutcome o;
    o.state = ParseOutcome::State::Error;
    o.error = std::move(msg);
    return o;
}

ParseOutcome HttpResponseParser::tryParse() {
    for (;;) {
        switch (state_) {
            case State::StatusLine: {
                std::string line;
                const int r = takeLine(pending_, line, kMaxRequestLine);
                if (r <= 0) return r < 0 ? fail("status line too long") : ParseOutcome{};
                // HTTP/1.1 200 OK
                const size_t sp1 = line.find(' ');
                const size_t sp2 = line.find(' ', sp1 + 1);
                if (sp1 == std::string::npos) return fail("malformed status line");
                const std::string version = line.substr(0, sp1);
                if (version.rfind("HTTP/1.", 0) != 0 || version.size() < 8) {
                    return fail("unsupported version in status line");
                }
                resp_.httpMinor = version[7] == '0' ? 0 : 1;
                const size_t codeEnd = sp2 == std::string::npos ? line.size() : sp2;
                const std::string codeStr = line.substr(sp1 + 1, codeEnd - sp1 - 1);
                if (codeStr.size() != 3) return fail("bad status code");
                int code = 0;
                for (char c : codeStr) {
                    if (c < '0' || c > '9') return fail("bad status code");
                    code = code * 10 + (c - '0');
                }
                resp_.status = code;
                state_ = State::Headers;
                continue;
            }
            case State::Headers: {
                std::string line;
                const int r = takeLine(pending_, line, kMaxHeaderBytes);
                if (r <= 0) return r < 0 ? fail("headers too large") : ParseOutcome{};
                if (line.empty()) {
                    // 1xx/204/304 无消息体：头部结束即完成（如 WebSocket 101）
                    if (resp_.status >= 100 && resp_.status < 200 ||
                        resp_.status == 204 || resp_.status == 304) {
                        state_ = State::Done;
                        continue;
                    }
                    const bool chunked =
                        resp_.headers.contains("Transfer-Encoding") &&
                        core::string_util::toLower(resp_.headers.get("Transfer-Encoding"))
                            .find("chunked") != std::string::npos;
                    const std::string cl = resp_.headers.get("Content-Length");
                    if (chunked) {
                        state_ = State::BodyChunkSize;
                    } else if (!cl.empty()) {
                        size_t n = 0;
                        bool ok = !cl.empty();
                        for (char c : cl) {
                            if (c < '0' || c > '9') { ok = false; break; }
                            n = n * 10 + static_cast<size_t>(c - '0');
                            if (n > (1ull << 40)) break;
                        }
                        if (!ok) return fail("bad Content-Length");
                        bodyRemaining_ = n;
                        state_ = bodyRemaining_ > 0 ? State::BodyIdentity : State::Done;
                    } else {
                        state_ = State::ReadUntilClose;
                    }
                    continue;
                }
                const size_t colon = line.find(':');
                if (colon == std::string::npos) return fail("malformed header line");
                resp_.headers.add(core::string_util::trim(line.substr(0, colon)),
                                  core::string_util::trim(line.substr(colon + 1)));
                continue;
            }
            case State::BodyIdentity: {
                const size_t take = pending_.size() < bodyRemaining_ ? pending_.size()
                                                                      : bodyRemaining_;
                if (take > 0) {
                    resp_.body.append(pending_, 0, take);
                    pending_.erase(0, take);
                    bodyRemaining_ -= take;
                }
                if (bodyRemaining_ == 0) state_ = State::Done;
                else return ParseOutcome{};
                continue;
            }
            case State::BodyChunkSize: {
                std::string line;
                const int r = takeLine(pending_, line, 64);
                if (r <= 0) return r < 0 ? fail("bad chunk size line") : ParseOutcome{};
                if (!parseChunkSize(line, chunkRemaining_)) return fail("bad chunk size");
                state_ = chunkRemaining_ == 0 ? State::BodyChunkTrail : State::BodyChunkData;
                continue;
            }
            case State::BodyChunkData: {
                const size_t take = pending_.size() < chunkRemaining_ ? pending_.size()
                                                                       : chunkRemaining_;
                if (take > 0) {
                    resp_.body.append(pending_, 0, take);
                    pending_.erase(0, take);
                    chunkRemaining_ -= take;
                }
                if (chunkRemaining_ == 0) state_ = State::BodyChunkCrlf;
                else return ParseOutcome{};
                continue;
            }
            case State::BodyChunkCrlf: {
                std::string line;
                const int r = takeLine(pending_, line, 0);
                if (r <= 0) return r < 0 ? fail("bad chunk framing") : ParseOutcome{};
                if (!line.empty()) return fail("bad chunk framing (expected CRLF)");
                state_ = State::BodyChunkSize;
                continue;
            }
            case State::BodyChunkTrail: {
                std::string line;
                const int r = takeLine(pending_, line, kMaxHeaderBytes);
                if (r <= 0) return r < 0 ? fail("bad chunk trailer") : ParseOutcome{};
                if (line.empty()) state_ = State::Done;
                continue; // 忽略 trailer 头
            }
            case State::ReadUntilClose: {
                resp_.body.append(pending_);
                pending_.clear();
                return ParseOutcome{}; // 由对端关闭判定完成
            }
            case State::Done: {
                ParseOutcome o;
                o.state = ParseOutcome::State::Complete;
                return o;
            }
        }
    }
}

} // namespace zenith::http
