#include "zenith/http/session.hpp"

#include <sstream>

namespace zenith::http {

// ---------------------------------------------------------------------------
// Session 序列化：key=value 按行（key/value 转义 \n 与 \\）
// ---------------------------------------------------------------------------
namespace {
std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '\n') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}
std::string unesc(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            out.push_back(s[++i]);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}
} // namespace

std::string Session::serialize() const {
    std::ostringstream os;
    for (const auto& [k, v] : data_) {
        os << esc(k) << '=' << esc(v) << '\n';
    }
    return os.str();
}

void Session::deserialize(const std::string& blob) {
    data_.clear();
    size_t pos = 0;
    while (pos < blob.size()) {
        const size_t nl = blob.find('\n', pos);
        const std::string line =
            blob.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? blob.size() : nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        data_[unesc(line.substr(0, eq))] = unesc(line.substr(eq + 1));
    }
}

// ---------------------------------------------------------------------------
// MemorySessionStore
// ---------------------------------------------------------------------------
MemorySessionStore::MemorySessionStore(size_t maxEntries, std::chrono::seconds ttl)
    : maxEntries_(maxEntries), ttl_(ttl) {}

std::string MemorySessionStore::load(const std::string& sid) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(sid);
    if (it == map_.end()) return "";
    it->second.lastUsed = std::chrono::steady_clock::now().time_since_epoch().count();
    sweepLocked();
    return it->second.blob;
}

void MemorySessionStore::save(const std::string& sid, const std::string& blob) {
    std::lock_guard<std::mutex> lk(mtx_);
    map_[sid] = Entry{blob, std::chrono::steady_clock::now().time_since_epoch().count()};
    sweepLocked();
}

void MemorySessionStore::remove(const std::string& sid) {
    std::lock_guard<std::mutex> lk(mtx_);
    map_.erase(sid);
}

size_t MemorySessionStore::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return map_.size();
}

void MemorySessionStore::sweepLocked() {
    // 写路径顺手清扫（低频触发：超容量或攒够 128 条过期）
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    const int64_t ttlNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(ttl_).count();
    static thread_local int sweepDebt = 0;
    if (++sweepDebt < 128 && map_.size() <= maxEntries_) return;
    sweepDebt = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (now - it->second.lastUsed > ttlNs) {
            it = map_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// SessionManager
// ---------------------------------------------------------------------------
SessionManager::SessionManager(std::shared_ptr<ISessionStore> store)
    : store_(std::move(store)) {}

std::string SessionManager::newSessionId() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* kHex = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (int i = 0; i < 16; ++i) {
        const uint64_t v = rng();
        id.push_back(kHex[(v >> 4) & 0xF]);
        id.push_back(kHex[v & 0xF]);
    }
    return id;
}

std::shared_ptr<Session> SessionManager::load(const std::string& cookieSid) {
    if (!cookieSid.empty() && cookieSid.size() == 32) {
        const std::string blob = store_->load(cookieSid);
        if (!blob.empty()) {
            auto s = std::make_shared<Session>(cookieSid);
            s->deserialize(blob);
            return s;
        }
    }
    auto s = std::make_shared<Session>(newSessionId());
    s->isNew_ = true;
    s->dirty_ = true; // 新建需要落库
    return s;
}

bool SessionManager::save(const std::shared_ptr<Session>& s) {
    if (!s->dirty()) return false;
    store_->save(s->id(), s->serialize());
    s->dirty_ = false;
    const bool wasNew = s->isNew_;
    s->isNew_ = false;
    return wasNew; // 新建 → 需要下发 cookie
}

// ---------------------------------------------------------------------------
// 中间件
// ---------------------------------------------------------------------------
Middleware sessionMiddleware(std::shared_ptr<SessionManager> mgr) {
    return [m = std::move(mgr)](Context& c) {
        auto s = m->load(c.cookie(SessionManager::kCookieName()));
        c.setContext(std::static_pointer_cast<void>(s));
        c.next();
        if (m->save(s)) {
            c.setHeader("Set-Cookie",
                        std::string(SessionManager::kCookieName()) + "=" + s->id() +
                            "; Path=/; HttpOnly; SameSite=Lax");
        }
    };
}

} // namespace zenith::http
