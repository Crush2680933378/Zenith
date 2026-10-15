// ============================================================================
//  zenith::http::session —— 会话管理（M5.1）
//  - Cookie 解析：Context::cookie()
//  - ISessionStore：存储抽象（内存实现默认；Redis 后端走 M3 自研客户端）
//  - sessionMiddleware(store)：读 zenith_sid → 加载/新建 → 响应后写回
// ============================================================================
#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>

#include "zenith/core/export.hpp"
#include "zenith/http/context.hpp"

namespace zenith::http {

class Session {
public:
    explicit Session(std::string id) : id_(std::move(id)) {}

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] bool isNew() const noexcept { return isNew_; }

    std::string get(const std::string& key, const std::string& def = "") const {
        auto it = data_.find(key);
        return it == data_.end() ? def : it->second;
    }
    void set(const std::string& key, std::string value) {
        data_[key] = std::move(value);
        dirty_ = true;
    }
    void remove(const std::string& key) {
        data_.erase(key);
        dirty_ = true;
    }
    [[nodiscard]] bool contains(const std::string& key) const { return data_.count(key) > 0; }
    [[nodiscard]] size_t size() const noexcept { return data_.size(); }

    /// 序列化（简单 kv 制；供存储后端使用）
    [[nodiscard]] std::string serialize() const;
    void deserialize(const std::string& blob);

    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

private:
    friend class SessionManager;
    std::string id_;
    std::map<std::string, std::string> data_;
    bool isNew_ = false;
    bool dirty_ = false;
};

/// 会话存储抽象
class ISessionStore {
public:
    virtual ~ISessionStore() = default;
    /// 读（不存在返回空串）
    virtual std::string load(const std::string& sid) = 0;
    /// 写（覆盖）
    virtual void save(const std::string& sid, const std::string& blob) = 0;
    /// 删除
    virtual void remove(const std::string& sid) = 0;
};

/// 内存存储（默认；带容量上限与简单过期清扫）
class ZENITH_API MemorySessionStore final : public ISessionStore {
public:
    explicit MemorySessionStore(size_t maxEntries = 100000,
                                std::chrono::seconds ttl = std::chrono::seconds(3600));
    std::string load(const std::string& sid) override;
    void save(const std::string& sid, const std::string& blob) override;
    void remove(const std::string& sid) override;
    [[nodiscard]] size_t size() const;

private:
    void sweepLocked();

    mutable std::mutex mtx_;
    struct Entry {
        std::string blob;
        int64_t lastUsed;
    };
    std::map<std::string, Entry> map_;
    size_t maxEntries_;
    std::chrono::seconds ttl_;
};

/// 会话管理器（生成 id、装载/保存）
class ZENITH_API SessionManager {
public:
    explicit SessionManager(std::shared_ptr<ISessionStore> store);

    /// 从请求装载（cookie 里的 sid；无效/缺失则新建）
    std::shared_ptr<Session> load(const std::string& cookieSid);

    /// 保存（新建会话返回 true —— 调用方应下发 Set-Cookie）
    bool save(const std::shared_ptr<Session>& s);

    [[nodiscard]] ISessionStore& store() const { return *store_; }
    [[nodiscard]] static constexpr const char* kCookieName() noexcept { return "zenith_sid"; }

private:
    static std::string newSessionId();

    std::shared_ptr<ISessionStore> store_;
};

/// 会话中间件：c.setContext 挂 Session；handler 内：
///   auto s = c.session();  s->set("user", "tom");
ZENITH_API Middleware sessionMiddleware(std::shared_ptr<SessionManager> mgr);

} // namespace zenith::http
