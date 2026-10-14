#include "zenith/http/router.hpp"

#include "zenith/core/string.hpp"

namespace zenith::http {

struct Router::Node {
    std::map<std::string, std::unique_ptr<Node>> statics; // 静态段 → 子节点
    std::unique_ptr<Node> param;                           // :name
    std::string paramName;
    std::unique_ptr<Node> catchAll;                        // *name（兜底剩余全段）
    std::string catchAllName;
    std::map<HttpMethod, RouteEntry> routes;               // 方法 → 处理链
};

Router::Router() : root_(std::make_unique<Node>()) {}
Router::~Router() = default;

namespace {
std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segs;
    for (const std::string& s : core::string_util::split(path, '/', false)) {
        segs.push_back(s);
    }
    return segs;
}
} // namespace

Router& Router::handle(HttpMethod m, const std::string& pattern, Handler h,
                       std::vector<Middleware> mw) {
    Node* node = insertNode(pattern);
    if (node == nullptr) return *this;
    node->routes[m] = RouteEntry{std::move(h), std::move(mw)};
    ++routeCount_;
    return *this;
}

Router& Router::use(Middleware mw) {
    globals_.push_back(std::move(mw));
    return *this;
}

Router::RouterGroup Router::group(const std::string& prefix, std::vector<Middleware> mw) {
    return RouterGroup(*this, prefix, std::move(mw));
}

Router::Node* Router::insertNode(const std::string& pattern) {
    const std::vector<std::string> segs = splitPath(pattern);
    Node* cur = root_.get();
    for (const std::string& seg : segs) {
        if (seg.empty()) continue;
        if (seg.size() >= 2 && seg[0] == '*') {
            if (cur->catchAll == nullptr) cur->catchAll = std::make_unique<Node>();
            cur->catchAllName = seg.substr(1);
            cur = cur->catchAll.get();
            break; // catchAll 必须是最后一段
        }
        if (!seg.empty() && seg[0] == ':') {
            if (cur->param == nullptr) cur->param = std::make_unique<Node>();
            cur->paramName = seg.substr(1);
            cur = cur->param.get();
            continue;
        }
        auto& child = cur->statics[seg];
        if (child == nullptr) child = std::make_unique<Node>();
        cur = child.get();
    }
    return cur;
}

const Router::Node* Router::walk(const std::string& path,
                                 std::unordered_map<std::string, std::string>& params,
                                 bool& pathExists) const {
    const std::vector<std::string> segs = splitPath(path);
    const Node* cur = root_.get();

    for (size_t i = 0; i < segs.size(); ++i) {
        const std::string& seg = segs[i];
        auto it = cur->statics.find(seg);
        if (it != cur->statics.end()) {
            cur = it->second.get();
            continue;
        }
        if (cur->param != nullptr) {
            params[cur->paramName] = seg;
            cur = cur->param.get();
            continue;
        }
        if (cur->catchAll != nullptr) {
            std::string rest;
            for (size_t j = i; j < segs.size(); ++j) {
                if (j > i) rest.push_back('/');
                rest += segs[j];
            }
            params[cur->catchAllName] = std::move(rest);
            cur = cur->catchAll.get();
            return cur; // 剩余段全部被吞并
        }
        return nullptr;
    }

    pathExists = cur != nullptr;
    return cur;
}

void Router::dispatch(Context& ctx) {
    std::unordered_map<std::string, std::string> params;
    bool pathExists = false;
    const Node* node = walk(ctx.path(), params, pathExists);

    if (node == nullptr || node->routes.empty()) {
        ctx.status(404).text(404, "404 page not found: " + ctx.path());
        return;
    }

    // GET 请求自动兼容 HEAD（未显式注册 HEAD 时）
    HttpMethod m = ctx.method();
    auto it = node->routes.find(m);
    if (it == node->routes.end()) {
        if (m == HttpMethod::Head) {
            it = node->routes.find(HttpMethod::Get);
        }
        if (it == node->routes.end()) {
            ctx.status(405).text(405, "405 method not allowed");
            return;
        }
    }

    // 组链：全局中间件 → 路由中间件 → handler
    const RouteEntry& entry = it->second;
    std::vector<Handler> chain;
    chain.reserve(globals_.size() + entry.middlewares.size() + 1);
    for (const auto& g : globals_) chain.push_back(g);
    for (const auto& r : entry.middlewares) chain.push_back(r);
    chain.push_back(entry.handler);

    // 写入路径参数后执行
    for (auto& [k, v] : params) {
        ctx.params_[std::move(k)] = std::move(v);
    }
    ctx.runChain(std::move(chain));
}

bool Router::hasRoute(const std::string& pattern) const {
    std::unordered_map<std::string, std::string> params;
    bool exists = false;
    const Node* node = walk(pattern, params, exists);
    return node != nullptr && !node->routes.empty();
}

Router::RouterGroup& Router::RouterGroup::handle(HttpMethod m, const std::string& pattern,
                                                 Handler h, std::vector<Middleware> extra) {
    std::vector<Middleware> all = mw_;
    for (auto& e : extra) all.push_back(std::move(e));
    std::string full = prefix_;
    if (!pattern.empty() && pattern[0] != '/') full.push_back('/');
    full += pattern;
    router_.handle(m, full, std::move(h), std::move(all));
    return *this;
}

} // namespace zenith::http
