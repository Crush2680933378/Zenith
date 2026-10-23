// ============================================================================
//  db_vector —— C ABI 插件：向量存储服务（M10.1）
//  自研 HNSW 索引按集合分实例；payload 原样存取（JSON 字符串）。
//  HTTP 层由宿主经 vector_api.hpp 的 mountVectorApi 挂载（Qdrant 兼容子集）。
// ============================================================================
#include <map>
#include <memory>
#include <new>
#include <shared_mutex>
#include <string>
#include <vector>

#include "zenith/abi/zenith_abi.h"

#include "zenith/vector/vector_service.hpp"

namespace {

const zenith_host_api* g_host = nullptr;

class VectorStoreImpl final : public zenith::vector::IVectorStoreService {
public:
    struct Collection {
        zenith::vector::CollectionConfig cfg;
        std::unique_ptr<zenith::vector::HnswIndex> index;
        std::unordered_map<uint64_t, std::string> payloads; // id → payload JSON
    };

    zenith::core::Result<void> createCollection(const std::string& name,
                                                const zenith::vector::CollectionConfig& cfg) override {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        if (cols_.count(name) > 0) {
            return zenith::core::Result<void>::Err(
                zenith::core::Error(210, "collection exists: " + name));
        }
        if (cfg.dim <= 0) {
            return zenith::core::Result<void>::Err(
                zenith::core::Error(211, "dim must be > 0"));
        }
        zenith::vector::HnswConfig hc;
        hc.dim = cfg.dim;
        hc.metric = cfg.metric;
        hc.m = cfg.m;
        hc.efConstruction = cfg.efConstruction;
        hc.efSearch = cfg.efSearch;
        Collection c;
        c.cfg = cfg;
        c.index = std::make_unique<zenith::vector::HnswIndex>(std::move(hc));
        cols_[name] = std::move(c);
        if (g_host != nullptr) {
            g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, "db_vector", 0,
                              ("collection created: " + name + " dim=" +
                               std::to_string(cfg.dim))
                                  .c_str());
        }
        return zenith::core::Result<void>::Ok();
    }

    zenith::core::Result<void> deleteCollection(const std::string& name) override {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        return cols_.erase(name) > 0
                   ? zenith::core::Result<void>::Ok()
                   : zenith::core::Result<void>::Err(
                         zenith::core::Error(212, "no such collection: " + name));
    }

    std::vector<zenith::vector::CollectionInfo> collections() override {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        std::vector<zenith::vector::CollectionInfo> out;
        out.reserve(cols_.size());
        for (const auto& [name, c] : cols_) {
            zenith::vector::CollectionInfo i;
            i.name = name;
            i.dim = c.cfg.dim;
            i.metric = c.cfg.metric;
            i.points = c.index->size();
            out.push_back(std::move(i));
        }
        return out;
    }

    zenith::core::Result<zenith::vector::CollectionInfo>
    collectionInfo(const std::string& name) override {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        auto it = cols_.find(name);
        if (it == cols_.end()) {
            return zenith::core::Result<zenith::vector::CollectionInfo>::Err(
                zenith::core::Error(212, "no such collection: " + name));
        }
        zenith::vector::CollectionInfo i;
        i.name = name;
        i.dim = it->second.cfg.dim;
        i.metric = it->second.cfg.metric;
        i.points = it->second.index->size();
        return zenith::core::Result<zenith::vector::CollectionInfo>::Ok(std::move(i));
    }

    zenith::core::Result<void> upsert(
        const std::string& name,
        const std::vector<zenith::vector::VectorPoint>& points) override {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        auto it = cols_.find(name);
        if (it == cols_.end()) {
            lk.unlock();
            return zenith::core::Result<void>::Err(
                zenith::core::Error(212, "no such collection: " + name));
        }
        for (const auto& p : points) {
            if (static_cast<int>(p.vec.size()) != it->second.cfg.dim) {
                return zenith::core::Result<void>::Err(
                    zenith::core::Error(213, "vector dim mismatch on id " +
                                                 std::to_string(p.id)));
            }
        }
        for (const auto& p : points) {
            it->second.index->upsert(p.id, p.vec.data());
            it->second.payloads[p.id] = p.payloadJson;
        }
        return zenith::core::Result<void>::Ok();
    }

    zenith::core::Result<void> remove(const std::string& name,
                                      const std::vector<uint64_t>& ids) override {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        auto it = cols_.find(name);
        if (it == cols_.end()) {
            return zenith::core::Result<void>::Err(
                zenith::core::Error(212, "no such collection: " + name));
        }
        for (uint64_t id : ids) {
            it->second.index->remove(id);
            it->second.payloads.erase(id);
        }
        return zenith::core::Result<void>::Ok();
    }

    zenith::core::Result<zenith::vector::VectorPoint> get(const std::string& name,
                                                          uint64_t id) override {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        auto it = cols_.find(name);
        if (it == cols_.end()) {
            return zenith::core::Result<zenith::vector::VectorPoint>::Err(
                zenith::core::Error(212, "no such collection: " + name));
        }
        zenith::vector::VectorPoint p;
        p.id = id;
        if (!it->second.index->get(id, p.vec)) {
            return zenith::core::Result<zenith::vector::VectorPoint>::Err(
                zenith::core::Error(214, "no such point"));
        }
        auto pit = it->second.payloads.find(id);
        if (pit != it->second.payloads.end()) p.payloadJson = pit->second;
        return zenith::core::Result<zenith::vector::VectorPoint>::Ok(std::move(p));
    }

    zenith::core::Result<std::vector<zenith::vector::ScoredPoint>>
    search(const std::string& name, const std::vector<float>& query, int k,
           float scoreThreshold) override {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        auto it = cols_.find(name);
        if (it == cols_.end()) {
            return zenith::core::Result<std::vector<zenith::vector::ScoredPoint>>::Err(
                zenith::core::Error(212, "no such collection: " + name));
        }
        if (static_cast<int>(query.size()) != it->second.cfg.dim) {
            return zenith::core::Result<std::vector<zenith::vector::ScoredPoint>>::Err(
                zenith::core::Error(213, "query dim mismatch"));
        }
        auto hits = it->second.index->search(query.data(), k);
        std::vector<zenith::vector::ScoredPoint> out;
        out.reserve(hits.size());
        for (const auto& h : hits) {
            if (h.score < scoreThreshold) continue;
            zenith::vector::ScoredPoint sp;
            sp.id = h.id;
            sp.score = h.score;
            auto pit = it->second.payloads.find(h.id);
            if (pit != it->second.payloads.end()) sp.payloadJson = pit->second;
            out.push_back(std::move(sp));
        }
        return zenith::core::Result<std::vector<zenith::vector::ScoredPoint>>::Ok(
            std::move(out));
    }

    size_t totalPoints() override {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        size_t n = 0;
        for (const auto& [_, c] : cols_) n += c.index->size();
        return n;
    }

private:
    mutable std::shared_mutex mtx_;
    std::map<std::string, Collection> cols_;
};

void destroyVectorStore(void* p) {
    delete static_cast<VectorStoreImpl*>(p);
}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "db_vector", 0,
                    "db_vector: init (self-hosted HNSW, Qdrant-compatible API via "
                    "mountVectorApi)");
    auto* svc = new (std::nothrow) VectorStoreImpl();
    if (svc == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  zenith::vector::IVectorStoreService::service_name(),
                                  zenith::vector::IVectorStoreService::service_version(),
                                  static_cast<void*>(svc), &destroyVectorStore);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) { g_host = nullptr; }

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "db_vector",
    "0.1.0",
    "Zenith vector store: self-hosted HNSW ANN index (RAG memory backbone)",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
