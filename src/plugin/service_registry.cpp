#include "zenith/plugin/service_registry.hpp"

namespace zenith::plugin {

ServiceRegistry::~ServiceRegistry() { clear(); }

zenith_status ServiceRegistry::registerTyped(const char* interfaceName, uint32_t implVersion,
                                             std::shared_ptr<void> service, bool replace) {
    if (interfaceName == nullptr || interfaceName[0] == '\0' || !service) {
        return ZENITH_ERR_INVALID_ARGUMENT;
    }
    std::unique_lock<std::shared_mutex> lk(mtx_);
    auto it = entries_.find(interfaceName);
    if (it != entries_.end() && !replace) return ZENITH_ERR_ALREADY_EXISTS;

    Entry e;
    e.version = implVersion;
    e.typed   = std::move(service);
    if (it != entries_.end()) {
        it->second.typed = std::move(e.typed);
        it->second.version = e.version;
        // 若原来是 raw 条目，先按旧 owner 销毁
        if (it->second.rawInstance != nullptr && it->second.rawDestroy != nullptr) {
            it->second.rawDestroy(it->second.rawInstance);
        }
        it->second.rawInstance = nullptr;
        it->second.rawDestroy  = nullptr;
        it->second.owner       = nullptr;
    } else {
        entries_.emplace(interfaceName, std::move(e));
    }
    return ZENITH_OK;
}

zenith_status ServiceRegistry::registerRaw(const char* interfaceName, uint32_t implVersion,
                                           void* instance, zenith_destroy_fn destroy,
                                           void* ownerToken, bool replace) {
    if (interfaceName == nullptr || interfaceName[0] == '\0' || instance == nullptr) {
        return ZENITH_ERR_INVALID_ARGUMENT;
    }
    std::unique_lock<std::shared_mutex> lk(mtx_);
    auto it = entries_.find(interfaceName);
    if (it != entries_.end() && !replace) return ZENITH_ERR_ALREADY_EXISTS;

    Entry e;
    e.version     = implVersion;
    e.rawInstance = instance;
    e.rawDestroy  = destroy;
    e.owner       = ownerToken;
    if (it != entries_.end()) {
        // 覆盖旧条目：raw 的调用旧 destroy，typed 的交给 shared_ptr
        if (it->second.rawInstance != nullptr && it->second.rawDestroy != nullptr) {
            it->second.rawDestroy(it->second.rawInstance);
        }
        it->second.typed       = nullptr;
        it->second.version     = e.version;
        it->second.rawInstance = e.rawInstance;
        it->second.rawDestroy  = e.rawDestroy;
        it->second.owner       = e.owner;
    } else {
        entries_.emplace(interfaceName, std::move(e));
    }
    return ZENITH_OK;
}

void* ServiceRegistry::queryRaw(const char* interfaceName, uint32_t minVersion,
                                zenith_status* status) const {
    if (interfaceName == nullptr || interfaceName[0] == '\0') {
        if (status) *status = ZENITH_ERR_INVALID_ARGUMENT;
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> lk(mtx_);
    auto it = entries_.find(interfaceName);
    if (it == entries_.end()) {
        if (status) *status = ZENITH_ERR_NOT_FOUND;
        return nullptr;
    }
    if (it->second.version < minVersion) {
        if (status) *status = ZENITH_ERR_VERSION_MISMATCH;
        return nullptr;
    }
    if (status) *status = ZENITH_OK;
    return it->second.typed ? it->second.typed.get() : it->second.rawInstance;
}

std::shared_ptr<void> ServiceRegistry::queryTyped(const char* interfaceName,
                                                  uint32_t minVersion) const {
    if (interfaceName == nullptr) return nullptr;
    std::shared_lock<std::shared_mutex> lk(mtx_);
    auto it = entries_.find(interfaceName);
    if (it == entries_.end() || it->second.version < minVersion) return nullptr;
    return it->second.typed; // raw 条目此处置空，typed 查询只认 typed
}

size_t ServiceRegistry::removeByOwner(void* ownerToken) {
    if (ownerToken == nullptr) return 0;
    std::vector<Entry> doomed;
    {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.owner == ownerToken) {
                doomed.push_back(std::move(it->second));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 在锁外 destroy：回调可能反过来查询注册表
    for (Entry& e : doomed) {
        if (e.rawInstance != nullptr && e.rawDestroy != nullptr) e.rawDestroy(e.rawInstance);
    }
    return doomed.size();
}

void ServiceRegistry::clear() {
    std::vector<Entry> doomed;
    {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        doomed.reserve(entries_.size());
        for (auto& [name, e] : entries_) doomed.push_back(std::move(e));
        entries_.clear();
    }
    for (Entry& e : doomed) {
        if (e.rawInstance != nullptr && e.rawDestroy != nullptr) e.rawDestroy(e.rawInstance);
    }
}

size_t ServiceRegistry::size() const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return entries_.size();
}

std::vector<std::pair<std::string, uint32_t>> ServiceRegistry::list() const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    std::vector<std::pair<std::string, uint32_t>> out;
    out.reserve(entries_.size());
    for (const auto& [name, e] : entries_) out.emplace_back(name, e.version);
    return out;
}

} // namespace zenith::plugin
