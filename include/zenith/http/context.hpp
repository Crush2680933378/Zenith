// ============================================================================
//  zenith::http::Context —— 请求上下文（Gin 风格）
//  - 中间件链：chain = [全局mw..., 路由mw..., handler]；ctx.next() 推进
//  - 不调用 next() 即短路（如 abort/鉴权失败）
// ============================================================================
#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "zenith/http/http_message.hpp"
#include "zenith/json/json.hpp"

namespace zenith::http {

class Context; // 别名需要引用它

using Handler = std::function<void(Context&)>;
/// 中间件：包裹后续处理（洋葱模型）；调用 ctx.next() 放行。
using Middleware = std::function<void(Context&)>;

class Context {
public:
    /// 请求体不是合法 JSON 时抛出（服务器捕获后回 400）。
    struct BadRequestBody : std::runtime_error {
        explicit BadRequestBody(const std::string& what) : std::runtime_error(what) {}
    };

    explicit Context(HttpRequest req) : req_(std::move(req)) {}

    /* ---- 请求侧 ---- */
    [[nodiscard]] HttpMethod method() const noexcept { return req_.method; }
    [[nodiscard]] const std::string& path() const noexcept { return req_.path; }
    [[nodiscard]] const HttpRequest& request() const noexcept { return req_; }
    [[nodiscard]] const std::string& peer() const noexcept { return req_.peer; }

    /// 路径参数（:id）；不存在返回空串。
    [[nodiscard]] std::string param(const std::string& key) const {
        const auto it = params_.find(key);
        return it == params_.end() ? std::string() : it->second;
    }
    /// 查询参数（?a=1）；不存在返回 def。
    [[nodiscard]] std::string query(const std::string& key,
                                    const std::string& def = "") const {
        const auto it = req_.query.find(key);
        return it == req_.query.end() ? def : it->second;
    }
    [[nodiscard]] std::string header(const std::string& key,
                                     const std::string& def = "") const {
        return req_.headers.get(key, def);
    }
    /// 解析请求体 JSON；失败抛 BadRequestBody。
    [[nodiscard]] json::JsonValue bodyJson() const {
        auto v = json::JsonValue::parse(req_.body);
        if (!v) throw BadRequestBody("invalid JSON body: " + v.error().message);
        return std::move(v).value();
    }
    [[nodiscard]] const std::string& body() const noexcept { return req_.body; }

    /* ---- 响应侧（链式） ---- */
    Context& status(int code) {
        resp_.status = code;
        return *this;
    }
    Context& setHeader(const std::string& key, const std::string& value) {
        resp_.headers.set(key, value);
        return *this;
    }
    [[nodiscard]] int responseStatus() const noexcept { return resp_.status; }

    Context& text(int code, std::string body) {
        resp_.status = code;
        resp_.headers.set("Content-Type", "text/plain; charset=utf-8");
        resp_.body = std::move(body);
        return *this;
    }
    Context& html(int code, std::string body) {
        resp_.status = code;
        resp_.headers.set("Content-Type", "text/html; charset=utf-8");
        resp_.body = std::move(body);
        return *this;
    }
    Context& json(int code, const json::JsonValue& v) {
        resp_.status = code;
        resp_.headers.set("Content-Type", "application/json; charset=utf-8");
        resp_.body = v.dump();
        return *this;
    }
    Context& noContent() {
        resp_.status = 204;
        resp_.body.clear();
        return *this;
    }
    /// 直接终止链并响应（如 401/403）。
    void abort(int code, std::string msg = "") {
        resp_.status = code;
        resp_.headers.set("Content-Type", "text/plain; charset=utf-8");
        resp_.body = std::move(msg);
        aborted_ = true;
    }

    /// 推进中间件链（标准洋葱模型：next() 返回后可做后置处理）。
    /// 中间件内再调 next() 放行后续；不调用即短路。
    void next() {
        if (aborted_) return;
        if (idx_ >= chain_.size()) return;
        auto h = chain_[idx_++]; // 拷贝，防回调中改动链
        h(*this);
    }

    [[nodiscard]] HttpResponse& response() noexcept { return resp_; }
    [[nodiscard]] bool aborted() const noexcept { return aborted_; }

private:
    friend class Router;
    friend class HttpServer;

    void runChain(std::vector<Handler> chain) {
        chain_ = std::move(chain);
        idx_ = 0;
        next();
    }

    HttpRequest req_;
    HttpResponse resp_;
    std::unordered_map<std::string, std::string> params_;
    std::vector<Handler> chain_;
    size_t idx_ = 0;
    bool aborted_ = false;
};

} // namespace zenith::http
