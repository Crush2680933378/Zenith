// ============================================================================
//  zenith::vector::IVectorStoreService —— 向量存储服务契约（M10.1）
//  宿主与 db_vector 插件之间的共享接口；HTTP 层由 vector_api.hpp 的
//  mountVectorApi 挂载（Qdrant 兼容子集），消费方也可直接 C++ 调用。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "zenith/core/result.hpp"
#include "zenith/plugin/service.hpp"
#include "zenith/vector/hnsw.hpp"

namespace zenith::vector {

struct CollectionInfo {
    std::string name;
    int dim = 0;
    Metric metric = Metric::Cosine;
    size_t points = 0;
};

struct VectorPoint {
    uint64_t id = 0;
    std::vector<float> vec;
    std::string payloadJson; // 任意业务载荷（原样存取）
};

struct ScoredPoint {
    uint64_t id = 0;
    float score = 0;
    std::string payloadJson;
};

struct CollectionConfig {
    int dim = 0;
    Metric metric = Metric::Cosine;
    int m = 16;
    int efConstruction = 200;
    int efSearch = 64;
};

class IVectorStoreService : public plugin::IService {
public:
    ZENITH_SERVICE_DECL(IVectorStoreService, "zenith.vector.IVectorStoreService", 1)

    /// 创建集合（重名 → Err）
    virtual core::Result<void> createCollection(const std::string& name,
                                                const CollectionConfig& cfg) = 0;
    virtual core::Result<void> deleteCollection(const std::string& name) = 0;
    [[nodiscard]] virtual std::vector<CollectionInfo> collections() = 0;
    [[nodiscard]] virtual core::Result<CollectionInfo> collectionInfo(const std::string& name) = 0;

    virtual core::Result<void> upsert(const std::string& name,
                                      const std::vector<VectorPoint>& points) = 0;
    virtual core::Result<void> remove(const std::string& name,
                                      const std::vector<uint64_t>& ids) = 0;
    [[nodiscard]] virtual core::Result<VectorPoint> get(const std::string& name,
                                                        uint64_t id) = 0;

    /// 检索：top-k + 可选分数下限；payloadOnly 控制是否带载荷
    [[nodiscard]] virtual core::Result<std::vector<ScoredPoint>>
    search(const std::string& name, const std::vector<float>& query, int k,
           float scoreThreshold = -1e30f) = 0;

    /// 引擎健康信息（points 总数等）
    [[nodiscard]] virtual size_t totalPoints() = 0;
};

} // namespace zenith::vector
