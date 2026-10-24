// ============================================================================
//  OAuth 2.0 授权码 + PKCE 客户端实现（M13 第四批）
// ============================================================================
#include "zenith/http/oauth.hpp"

#include <random>

#include "zenith/ai/provider_router.hpp" // simpleHttpRequest（http/https）
#include "zenith/crypto/base64.hpp"
#include "zenith/crypto/sha256.hpp"
#include "zenith/json/json.hpp"

namespace zenith::http::oauth {
namespace {

std::mt19937_64& rngGen() {
    static thread_local std::mt19937_64 r{std::random_device{}()};
    return r;
}
uint64_t rnd64() { return rngGen()(); }

std::string urlEncode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

std::string base64UrlNoPad(const uint8_t* data, size_t n) {
    std::string s = crypto::base64Encode(data, n);
    while (!s.empty() && s.back() == '=') s.pop_back();
    for (char& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return s;
}

TokenResponse parseTokenBody(const ai::SimpleHttpResult& res) {
    TokenResponse t;
    if (!res.ok) {
        t.error = "http " + std::to_string(res.status) + ": " + res.body.substr(0, 200);
        return t;
    }
    auto v = json::JsonValue::parse(res.body);
    if (!v) {
        t.error = "malformed token response";
        return t;
    }
    t.accessToken = v.value()["access_token"].asString();
    t.refreshToken = v.value()["refresh_token"].asString();
    t.tokenType = v.value()["token_type"].asString();
    t.expiresInSeconds = v.value()["expires_in"].asInt(0);
    t.scope = v.value()["scope"].asString();
    t.ok = !t.accessToken.empty();
    if (!t.ok) {
        t.error = v.value()["error"].asString().empty()
                      ? "no access_token in response"
                      : v.value()["error"].asString();
    }
    return t;
}

TokenResponse postForm(const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& form,
                       int timeoutMs) {
    std::string body;
    for (size_t i = 0; i < form.size(); ++i) {
        if (i > 0) body.push_back('&');
        body += urlEncode(form[i].first) + "=" + urlEncode(form[i].second);
    }
    ai::SimpleHttpCall call;
    call.method = "POST";
    call.url = url;
    call.body = body;
    call.headers = {{"Content-Type", "application/x-www-form-urlencoded"},
                    {"Accept", "application/json"}};
    call.timeoutMs = timeoutMs;
    return parseTokenBody(simpleHttpRequest(call));
}

} // namespace

Pkce makePkce() {
    // 43~128 urlsafe；取 64 字符（48 字节熵 → base64url 64 字符）
    static const char* kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    Pkce p;
    p.verifier.reserve(64);
    for (int i = 0; i < 64; ++i) p.verifier.push_back(kAlpha[rnd64() % 66]);
    uint8_t digest[32];
    crypto::Sha256::raw(p.verifier, digest);
    p.challenge = base64UrlNoPad(digest, 32); // 43 字符无填充
    return p;
}

std::string randomState() {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; ++i) {
        const uint64_t v = rnd64();
        out.push_back(kHex[v & 0xF]);
        out.push_back(kHex[(v >> 4) & 0xF]);
    }
    return out;
}

std::string buildAuthorizeUrl(const OAuthClientConfig& cfg, const std::string& state,
                              const Pkce& pkce,
                              const std::vector<std::pair<std::string, std::string>>& extra) {
    std::string sep = cfg.authorizeUrl.find('?') == std::string::npos ? "?" : "&";
    std::string url = cfg.authorizeUrl + sep +
                      "response_type=code&client_id=" + urlEncode(cfg.clientId) +
                      "&redirect_uri=" + urlEncode(cfg.redirectUri) +
                      "&state=" + urlEncode(state) +
                      "&code_challenge=" + urlEncode(pkce.challenge) +
                      "&code_challenge_method=S256";
    if (!cfg.scope.empty()) url += "&scope=" + urlEncode(cfg.scope);
    for (const auto& [k, v] : extra) url += "&" + urlEncode(k) + "=" + urlEncode(v);
    return url;
}

TokenResponse exchangeCode(const OAuthClientConfig& cfg, const std::string& code,
                           const Pkce& pkce, int timeoutMs) {
    std::vector<std::pair<std::string, std::string>> form{
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", cfg.redirectUri},
        {"client_id", cfg.clientId},
        {"code_verifier", pkce.verifier}};
    if (!cfg.clientSecret.empty()) form.emplace_back("client_secret", cfg.clientSecret);
    return postForm(cfg.tokenUrl, form, timeoutMs);
}

TokenResponse refresh(const OAuthClientConfig& cfg, const std::string& refreshToken,
                      int timeoutMs) {
    std::vector<std::pair<std::string, std::string>> form{
        {"grant_type", "refresh_token"},
        {"refresh_token", refreshToken},
        {"client_id", cfg.clientId}};
    if (!cfg.clientSecret.empty()) form.emplace_back("client_secret", cfg.clientSecret);
    return postForm(cfg.tokenUrl, form, timeoutMs);
}

} // namespace zenith::http::oauth
