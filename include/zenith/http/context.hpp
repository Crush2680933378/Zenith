// ============================================================================
//  zenith::http::Context —— 请求上下文（Gin 风格）
//  - 中间件链：chain = [全局mw..., 路由mw..., handler]；ctx.next() 推进
//  - 不调用 next() 即短路（如 abort/鉴权失败）
// ============================================================================
#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "zenith/http/http_message.hpp"
#include "zenith/http/multipart.hpp"
#include "zenith/http/sse.hpp"
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

    /* ---- Cookie（M5.1） ---- */
    /// Cookie 值（解析 Cookie 头；不存在返回 def）。值为 urlDecode 后。
    [[nodiscard]] std::string cookie(const std::string& name,
                                     const std::string& def = "") const;

    /* ---- multipart 表单（M5.1；懒解析缓存） ---- */
    /// 表单字段值（非文件部分）；缺失返回空。
    [[nodiscard]] std::string form(const std::string& name) const;
    /// 文件部分（filename 非空的第一个同名部分）；无返回 nullptr。
    [[nodiscard]] const multipart::FormPart* file(const std::string& name) const;
    /// 全部部分（含普通字段与文件）
    [[nodiscard]] const std::vector<multipart::FormPart>& formParts() const;

    /* ---- 会话（sessionMiddleware 注入；未使用中间件时为空） ---- */
    [[nodiscard]] std::shared_ptr<void> contextPtr() const { return context_; }
    template <typename T>
    [[nodiscard]] std::shared_ptr<T> contextAs() const {
        return std::static_pointer_cast<T>(context_);
    }
    /// 便捷：sessionMiddleware 装配后可用。未装配抛 BadRequestBody 语义错误。
    [[nodiscard]] std::shared_ptr<class Session> session() const;
    void setContext(std::shared_ptr<void> ctx) { context_ = std::move(ctx); }

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

    /* ---- 流式响应（M9.1）---- */
    /// 切换为 SSE 流：立即写响应头并返回写端，此后服务器不再自动发送响应。
    /// 未绑定连接（直接构造 Context 的单测场景）返回 nullptr。
    std::shared_ptr<SseStream> sse(int code = 200, const SseStream::Headers& extraHeaders = {});
    /// 切换为通用 chunked 流（大响应/边算边传）。
    std::shared_ptr<ChunkWriter> chunkedStream(
        const std::string& contentType = "application/octet-stream");
    [[nodiscard]] bool streaming() const noexcept { return streaming_; }
    /// 由 HttpServer 在派发前注入（用户代码无需调用）。
    void setConnection(std::shared_ptr<net::TcpConnection> conn) { conn_ = std::move(conn); }

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
    std::shared_ptr<void> context_;                      // session 等附带状态
    mutable std::shared_ptr<std::vector<multipart::FormPart>> formCache_;
    std::shared_ptr<net::TcpConnection> conn_;            // 流式响应写端（M9.1）
    bool streaming_ = false;
    size_t idx_ = 0;
    bool aborted_ = false;
};

} // namespace zenith::http
