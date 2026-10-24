// ============================================================================
//  zenith::http::oauth —— OAuth 2.0 授权码 + PKCE 客户端（M13 第四批）
//
//  本服务作为 CLIENT 接入外部 IdP（GitHub/Google/自建）：
//    1. makePkce()                生成 verifier + S256 challenge
//    2. buildAuthorizeUrl()       跳转授权页（state 防 CSRF）
//    3. 回调带 code → exchangeCode()  换 access_token（含 code_verifier）
//    4. refresh()                 刷新令牌
//  令牌校验：JWT bearer 见 jwt.hpp（HS256/RS256 已有）；本模块只管换取流。
//  URL 编码 / 表单编码 / state 随机数全部自研。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::http::oauth {

struct ZENITH_API OAuthClientConfig {
    std::string authorizeUrl; // IdP 授权端点
    std::string tokenUrl;     // IdP 换令牌端点
    std::string clientId;
    std::string clientSecret; // 公共客户端（PKCE only）可空
    std::string redirectUri;
    std::string scope;
};

struct ZENITH_API Pkce {
    std::string verifier;    // 43-128 urlsafe 字符
    std::string challenge;   // base64url(SHA256(verifier)) 无填充（S256）
    bool valid() const { return verifier.size() >= 43 && challenge.size() == 43; }
};

struct ZENITH_API TokenResponse {
    bool ok = false;
    std::string accessToken;
    std::string refreshToken;
    std::string tokenType;
    int64_t expiresInSeconds = 0;
    std::string scope;
    std::string error;
};

/// PKCE 对（S256）；内部加密随机源
ZENITH_API Pkce makePkce();

/// 防 CSRF state（32 字节随机 hex）
ZENITH_API std::string randomState();

/// 拼授权跳转 URL：client_id/redirect_uri/response_type=code/scope/state/
/// code_challenge(+method=S256)；extra 附加自定义参数（如 access_type）
ZENITH_API std::string buildAuthorizeUrl(
    const OAuthClientConfig& cfg, const std::string& state, const Pkce& pkce,
    const std::vector<std::pair<std::string, std::string>>& extra = {});

/// 授权码换令牌（POST tokenUrl，application/x-www-form-urlencoded）。
/// timeoutMs 默认 15s。
ZENITH_API TokenResponse exchangeCode(const OAuthClientConfig& cfg,
                                      const std::string& code, const Pkce& pkce,
                                      int timeoutMs = 15000);

/// refresh_token 换新 access_token
ZENITH_API TokenResponse refresh(const OAuthClientConfig& cfg,
                                 const std::string& refreshToken,
                                 int timeoutMs = 15000);

} // namespace zenith::http::oauth
