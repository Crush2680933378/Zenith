// ============================================================================
//  zenith::http::Router —— 路由树（静态段 > :param > *catchAll 优先级）
//  - 模式：/users/:id/posts、/files/*filepath、/healthz
//  - 匹配语义：同一节点上静态子段优先；param 次之；catchAll 兜底整段剩余
//  - 路径存在但方法不同 → MethodNotAllowed(405)
// ============================================================================
#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/http/context.hpp"

namespace zenith::http {

class ZENITH_API Router {
public:
    Router();
    ~Router();

    // ---- 注册 ----
    Router& handle(HttpMethod m, const std::string& pattern, Handler h,
                   std::vector<Middleware> mw = {});
    Router& get(const std::string& p, Handler h, std::vector<Middleware> mw = {}) {
        return handle(HttpMethod::Get, p, std::move(h), std::move(mw));
    }
    Router& post(const std::string& p, Handler h, std::vector<Middleware> mw = {}) {
        return handle(HttpMethod::Post, p, std::move(h), std::move(mw));
    }
    Router& put(const std::string& p, Handler h, std::vector<Middleware> mw = {}) {
        return handle(HttpMethod::Put, p, std::move(h), std::move(mw));
    }
    Router& delete_(const std::string& p, Handler h, std::vector<Middleware> mw = {}) {
        return handle(HttpMethod::Delete, p, std::move(h), std::move(mw));
    }
    Router& patch(const std::string& p, Handler h, std::vector<Middleware> mw = {}) {
        return handle(HttpMethod::Patch, p, std::move(h), std::move(mw));
    }
    Router& head(const std::string& p, Handler h, std::vector<Middleware> mw = {}) {
        return handle(HttpMethod::Head, p, std::move(h), std::move(mw));
    }
    Router& options(const std::string& p, Handler h, std::vector<Middleware> mw = {}) {
        return handle(HttpMethod::Options, p, std::move(h), std::move(mw));
    }

    /// 全局中间件（先于路由级执行）。
    Router& use(Middleware mw);

    /// 分组：前缀 + 组级中间件（注册糖，匹配仍是全局单树）。
    class RouterGroup;

    RouterGroup group(const std::string& prefix, std::vector<Middleware> mw = {});

    // ---- 匹配与执行 ----
    /// 命中 → 组链执行；未命中路径 → 404；路径命中方法未命中 → 405。
    void dispatch(Context& ctx);

    [[nodiscard]] bool hasRoute(const std::string& pattern) const;

private:
    struct Node;
    struct RouteEntry {
        Handler handler;
        std::vector<Middleware> middlewares;
    };

    Node* insertNode(const std::string& pattern);
    const Node* walk(const std::string& path,
                     std::unordered_map<std::string, std::string>& params,
                     bool& pathExists) const;

    std::unique_ptr<Node> root_;
    std::vector<Middleware> globals_;
    size_t routeCount_ = 0;
};

/// 路由组（持有前缀与中间件；注册时拼接回根 Router）。
class ZENITH_API Router::RouterGroup {
public:
    RouterGroup(Router& r, std::string prefix, std::vector<Middleware> mw)
        : router_(r), prefix_(std::move(prefix)), mw_(std::move(mw)) {}

    RouterGroup& handle(HttpMethod m, const std::string& pattern, Handler h,
                        std::vector<Middleware> extra = {});
    RouterGroup& get(const std::string& p, Handler h, std::vector<Middleware> e = {}) {
        return handle(HttpMethod::Get, p, std::move(h), std::move(e));
    }
    RouterGroup& post(const std::string& p, Handler h, std::vector<Middleware> e = {}) {
        return handle(HttpMethod::Post, p, std::move(h), std::move(e));
    }
    RouterGroup& put(const std::string& p, Handler h, std::vector<Middleware> e = {}) {
        return handle(HttpMethod::Put, p, std::move(h), std::move(e));
    }
    RouterGroup& delete_(const std::string& p, Handler h, std::vector<Middleware> e = {}) {
        return handle(HttpMethod::Delete, p, std::move(h), std::move(e));
    }
    RouterGroup& use(Middleware mw) {
        mw_.push_back(std::move(mw));
        return *this;
    }

private:
    Router& router_;
    std::string prefix_;
    std::vector<Middleware> mw_;
};

} // namespace zenith::http
