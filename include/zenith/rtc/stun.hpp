// ============================================================================
//  zenith::rtc —— WebRTC 传输层基础件（RFC 5389 STUN + host ICE 候选）
//
//  范围（有意收窄，避免宣称"支持 WebRTC"）：
//    已做：STUN 消息编解码（Binding Request/Response、XOR-MAPPED-ADDRESS、
//          SOFTWARE、ERROR-CODE）、同步 Binding 客户端、host 候选收集
//    未做：ICE 连通性检查/TURN/DTLS/ SRTP/SDP —— 这些是独立的工程量，
//          当前不做也不会假装做了
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::rtc {

constexpr uint32_t kStunMagicCookie = 0x2112A442u;
constexpr uint16_t kStunTypeBindingRequest = 0x0001;
constexpr uint16_t kStunTypeBindingSuccess = 0x0101;
constexpr uint16_t kStunTypeBindingError = 0x0111;

struct StunAddress {
    std::string ip;  // 点分十进制（IPv4）
    uint16_t port = 0;
};

struct StunMessage {
    uint16_t type = kStunTypeBindingRequest;
    uint8_t transactionId[12] = {};
    std::optional<StunAddress> xorMapped;
    std::optional<StunAddress> mapped;
    std::string software;
    int errorCode = 0;
    std::string errorReason;
};

/// 编码 Binding Request（随机事务 ID 写入 outTxn）
ZENITH_API std::string encodeBindingRequest(uint8_t outTxn[12]);

/// 编码 Binding Response（成功），mapped 以 XOR-MAPPED-ADDRESS 形式写入
ZENITH_API std::string encodeBindingResponse(const StunAddress& mapped, const uint8_t txn[12]);

/// 编码 Binding Error Response（errorCode 形如 400/401）
ZENITH_API std::string encodeBindingError(int errorCode, const std::string& reason,
                                          const uint8_t txn[12]);

/// 解析一条 STUN 消息（校验长度/魔数/属性边界）
ZENITH_API bool decode(std::string_view data, StunMessage& out, std::string* err = nullptr);

/* --------------------------- 同步 Binding 客户端 --------------------------- */

class ZENITH_API StunClient {
public:
    /// 向 host:port 发 Binding Request 并解析响应。
    /// 返回 false 表示超时/网络错误/被服务端拒绝（reason 里给原因）。
    static bool binding(const std::string& host, uint16_t port, StunAddress& mapped,
                        int timeoutMs = 1000, std::string* err = nullptr);
};

/* ------------------------------ ICE 候选 ------------------------------ */

struct IceCandidate {
    std::string ip;
    uint16_t port = 0;
    std::string type = "host";  // 只有 host：srflx/relay 需要 STUN/TURN 服务器
};

/// 收集本机 host 候选（去重；空列表表示枚举失败）
ZENITH_API std::vector<IceCandidate> gatherHostCandidates();

}  // namespace zenith::rtc
