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
#include <functional>
#include <memory>
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
    // ---- ICE 连通性检查用到的属性 ----
    std::string username;                    // USERNAME（"ufrag1:ufrag2"）
    std::optional<uint32_t> priority;        // PRIORITY
    bool useCandidate = false;               // USE-CANDIDATE
    std::optional<uint64_t> iceControlling;  // ICE-CONTROLLING
    std::optional<uint64_t> iceControlled;   // ICE-CONTROLLED
};

/// ICE 检查请求的可选属性
struct IceCheckAttrs {
    std::string username;
    uint32_t priority = 0;
    bool useCandidate = false;
    bool controlling = true;  // true → ICE-CONTROLLING，false → ICE-CONTROLLED
    uint64_t tieBreaker = 0;  // controlling 时作为 role 值
};

/// 编码带 ICE 属性的 Binding Request（random txn 写入 outTxn）
ZENITH_API std::string encodeIceCheck(const IceCheckAttrs& attrs, uint8_t outTxn[12]);

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

/* ---------------------------- ICE 连通性检查 ---------------------------- */

/// 一条通过检查的候选对
struct IcePair {
    StunAddress remote;
    bool nominated = false;  // 收到带 USE-CANDIDATE 的请求
    bool responded = false;  // 我方请求得到响应
};

/// 极简 ICE agent：UDP socket 上做 STUN Binding 双向检查。
///
/// 已做：候选互检（USERNAME/PRIORITY/USE-CANDIDATE/ICE-CONTROLLING 属性）、
///       被动应答、有效对记录
/// 未做：候选优先级排序与提名收敛、TURN、ICE restart
class ZENITH_API IceAgent {
public:
    IceAgent();
    ~IceAgent();

    /// 绑定本地 UDP 端口（0 = 随机）
    bool bind(uint16_t port = 0);
    [[nodiscard]] uint16_t localPort() const;

    void addRemoteCandidate(const StunAddress& addr);

    /// 主动向所有远端候选发检查（每轮 timeoutMs），返回本轮成功的对
    std::vector<IcePair> runChecks(int timeoutMs = 500);

    /// 截至目前所有有效对（含被动接收到的提名）
    [[nodiscard]] std::vector<IcePair> validPairs() const;

    /// 是否至少有一条被提名的对（可用来判定"连通"）
    [[nodiscard]] bool nominated() const;

    /// 已提名的对（无则空）。WebRTC 就是用它选定的链路跑 DTLS/SRTP
    [[nodiscard]] std::optional<IcePair> selectedPair() const;

    /// 底层 UDP socket（把 DTLS 复用到同一条 ICE 链路上；调用方不得关闭）
    [[nodiscard]] int socketFd() const;

    /// 非 STUN 数据报的接管者（WebRTC：交给 DTLS 的 memory-BIO 模式）。
    /// 收到首字节不属于 STUN 的包时回调，由 sink 决定如何继续派发。
    using DatagramSink = std::function<void(std::string_view, const StunAddress& from)>;
    void setDatagramSink(DatagramSink sink);

    /// 直接发送一个数据报（DTLS 的出站字节走这里）
    bool sendTo(const StunAddress& to, std::string_view data);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zenith::rtc
