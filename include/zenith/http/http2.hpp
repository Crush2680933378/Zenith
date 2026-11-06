// ============================================================================
//  zenith::http::h2 —— HTTP/2 基础层（M7.7 第一刀）
//
//  这一层只做「字节 ↔ 帧 / 头块」，不碰连接与流状态机：
//    - 帧编解码（RFC 9113 §4）：9 字节头 + 载荷
//    - HPACK（RFC 7541）：静态表 + 索引/字面量/Dynamic Table Size Update
//
//  当前边界（有意为之，写在注释里避免误用）：
//    - HPACK **不做 Huffman 解码**：收到 H=1 的字符串返回压缩错误
//      （自家客户端默认不启用；接浏览器/curl 前必须补 Huffman 表）
//    - 动态表只实现插入/查找/清空，不做驱逐（表大小默认 4096，够跑通）
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::http::h2 {

enum class FrameType : uint8_t {
    Data = 0x0,
    Headers = 0x1,
    Priority = 0x2,
    RstStream = 0x3,
    Settings = 0x4,
    PushPromise = 0x5,
    Ping = 0x6,
    GoAway = 0x7,
    WindowUpdate = 0x8,
    Continuation = 0x9,
};

enum class ErrorCode : uint16_t {
    NoError = 0x0,
    ProtocolError = 0x1,
    InternalError = 0x2,
    FlowControlError = 0x3,
    SettingsTimeout = 0x4,
    StreamClosed = 0x5,
    FrameSizeError = 0x6,
    RefusedStream = 0x7,
    Cancel = 0x8,
    CompressionError = 0x9,
    ConnectError = 0xa,
    EnhanceYourCalm = 0xb,
    InadequateSecurity = 0xc,
    Http11Required = 0xd,
};

struct Frame {
    FrameType type = FrameType::Data;
    uint8_t flags = 0;
    uint32_t streamId = 0;  // 31 位（最高位保留）
    std::string payload;
};

constexpr size_t kFrameHeaderLen = 9;
constexpr uint32_t kDefaultMaxFrameSize = 16384;
constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/// 序列化一帧（长度前缀自动算；streamId 只取低 31 位）
ZENITH_API std::string encodeFrame(const Frame& f);

struct ParseResult {
    enum class Status { Pending, Ok, Error };
    Status status = Status::Pending;
    Frame frame;
    size_t consumed = 0;
    ErrorCode error = ErrorCode::NoError;
};

/// 从字节流解析一帧。数据不足返回 Pending（不消费）；超长/非法返回 Error。
ZENITH_API ParseResult parseFrame(std::string_view buf,
                                  uint32_t maxFrameSize = kDefaultMaxFrameSize);

/* ------------------------------ HPACK ------------------------------ */

/// 静态表条目数（RFC 7541 附录 A，共 61 条）
ZENITH_API size_t hpackStaticTableSize();
ZENITH_API const std::pair<std::string, std::string>& hpackStaticEntry(size_t idx);  // 1-based

class ZENITH_API HpackEncoder {
public:
    /// 编码一组头字段；返回头块字节（可直接放进 HEADERS 帧载荷）
    std::string encode(const std::vector<std::pair<std::string, std::string>>& headers);

private:
    std::vector<std::pair<std::string, std::string>> dynamic_;  // 新插入在前
    size_t dynamicSize_ = 0;
    size_t maxDynamicSize_ = 4096;

    size_t findName(const std::string& name) const;                        // 0 = 无
    size_t findFull(const std::string& name, const std::string& v) const;  // 0 = 无
    void insert(const std::string& name, const std::string& value);
    void evictTo(size_t limit);
};

class ZENITH_API HpackDecoder {
public:
    /// 解码头块；失败返回 false 并填 err（含位置信息，便于对照压缩错误）
    bool decode(std::string_view block, std::vector<std::pair<std::string, std::string>>& out,
                std::string* err = nullptr);

private:
    std::vector<std::pair<std::string, std::string>> dynamic_;
    size_t dynamicSize_ = 0;
    size_t maxDynamicSize_ = 4096;

    bool literal(std::string_view block, size_t& pos, std::string& name, std::string& value,
                 std::string* err);
    bool indexedName(std::string_view block, size_t& pos, size_t idx, std::string& name,
                     std::string* err);
    void insert(const std::string& name, const std::string& value);
    void evictTo(size_t limit);
};

/* --------------------------- h2c 服务端连接 --------------------------- */

/// 一次 h2 请求（已解出伪头）
struct H2Request {
    std::string method;
    std::string path;
    std::string authority;
    std::string scheme;
    std::vector<std::pair<std::string, std::string>> headers;  // 不含伪头
    std::string body;
};

struct H2Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

/// h2c（明文、prior-knowledge）服务端连接。
///
/// 这一层的职责边界：
///   - 处理客户端 preface、SETTINGS 交换/ACK、PING ACK、GOAWAY
///   - HEADERS(+CONTINUATION) → HPACK 解码 → 调 handler → 回 HEADERS + DATA
///   - **不做流控**（不会发 WINDOW_UPDATE，响应体需 < 64KB 默认窗口）
///   - **不支持 Huffman**（除非对端也是本库客户端）
class ZENITH_API H2ServerConnection {
public:
    using Handler = std::function<void(const H2Request&, H2Response&)>;

    explicit H2ServerConnection(Handler h);

    /// 喂入字节，返回要写回对端的字节（可能为空）
    std::string feed(std::string_view data);

    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    void handleFrame(const Frame& f, std::string& out);
    void dispatch(const H2Request& req, uint32_t streamId, std::string& out);
    void sendGoAway(std::string& out, ErrorCode code);

    Handler handler_;
    HpackDecoder decoder_;
    HpackEncoder encoder_;
    std::string in_;
    std::map<uint32_t, H2Request> streams_;  // 流 → 已收头字段/正文累积
    std::string pendingHeaderBlock_;         // CONTINUATION 拼装
    uint32_t pendingHeaderStream_ = 0;
    bool prefaceSeen_ = false;
    bool settingsSent_ = false;
    bool closed_ = false;
    std::string error_;
};

}  // namespace zenith::http::h2
