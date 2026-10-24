// ============================================================================
//  zenith::ai::GuardInspector —— LLM 输入安全护栏（M13 第二批 / ai_guard）
//
//  纯本地规则检测（零依赖、微秒级、无正则引擎）：对外部不可信文本做
//  两类扫描，供网关在调用推理/编排前把关：
//    1. 提示注入：指令劫持短语（大小写不敏感子串）——"ignore previous
//       instructions" / 越狱人设 / 系统提示刺探等
//    2. PII：邮箱 / 中国大陆手机号 / 身份证号（含出生日期校验）/ 银行卡
//       （Luhn）/ API 密钥（sk- / sk-ant- / AIza / ghp_ 前缀）
//  输出：风险分（0-100）+ 逐项 findings；超过 blockScore 判 blocked。
//  redact() 对 PII 掩码（保留首尾各 2 字符），供日志/落库使用。
//
//  定位：第一道便宜且确定的防线；语义级深检（向量相似度 + LLM 复核）
//  由上层 ai_guard 插件按需叠加（本接口预留 extraPhrases/allowlist）。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::http {
class Router;
} // namespace zenith::http

namespace zenith::ai {

struct GuardFinding {
    std::string type;  // prompt_injection / pii_email / pii_phone / pii_cn_id /
                       // pii_card / pii_apikey
    std::string match; // 命中片段（截断 40 字符）
    int score = 0;     // 该项风险分
};

struct ZENITH_API GuardResult {
    bool blocked = false;
    int score = 0; // 0-100（多项取封顶累计）
    std::vector<GuardFinding> findings;

    [[nodiscard]] bool hasFinding(const std::string& type) const;
};

class ZENITH_API GuardInspector {
public:
    struct Config {
        int blockScore = 60;                 // ≥ 该分阻断
        std::vector<std::string> extraPhrases; // 追加注入短语
        std::vector<std::string> allowlist;    // 白名单片段（命中即跳过该 finding）
    };

    GuardInspector() : GuardInspector(Config{}) {}
    explicit GuardInspector(Config cfg);

    /// 扫描不可信文本（用户输入 / RAG 检索段 / 工具返回）
    [[nodiscard]] GuardResult inspect(const std::string& text) const;

    /// PII 掩码（不判风险分；与 inspect 同一套 PII 检测器）
    [[nodiscard]] std::string redact(const std::string& text) const;

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

private:
    Config cfg_;
};

/// 跨插件服务契约（ai_guard 插件注册；同工具链 C++ 契约）
class ZENITH_API IGuardService {
public:
    virtual ~IGuardService() = default;
    static const char* service_name() { return "zenith.ai.guard"; }
    static uint32_t service_version() { return 1; }
    virtual GuardResult inspect(const std::string& text) = 0;
    virtual std::string redact(const std::string& text) = 0;
};

/// 默认实现：直接包一个 GuardInspector（插件/宿主共用）
class ZENITH_API GuardServiceImpl final : public IGuardService {
public:
    explicit GuardServiceImpl(GuardInspector inspector) : g_(std::move(inspector)) {}
    GuardResult inspect(const std::string& text) override { return g_.inspect(text); }
    std::string redact(const std::string& text) override { return g_.redact(text); }

private:
    GuardInspector g_;
};

/// 网关挂载：POST /guard/inspect {"text":...} → GuardResult JSON；
///           POST /guard/redact  {"text":...} → {"text":"掩码后"}
ZENITH_API void mountGuardApi(http::Router& r, const GuardInspector& g);

} // namespace zenith::ai
