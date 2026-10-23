// ============================================================================
//  zenith::vector::HnswIndex —— 自研 HNSW 近似最近邻索引（Malkov & Yashunin 2016）
//
//  结构：多层跳表式图。插入时按指数分布抽层号；自顶向下贪心定位，
//  每层用 ef 束宽搜索选 M 个邻居（启发式选邻 + 保底填充），
//  双向连边后按 M_max 剪枝。查询自顶向下贪心，第 0 层 ef_search 束宽取 top-k。
//
//  度量：
//    Cosine   —— 插入时预归一化，距离 = 1 - dot（dot 相似度越大距离越小）
//    Dot      —— 距离 = -dot（越大越近）
//    Euclidean—— 标准 L2
//
//  并发：shared_mutex——插入/删除独占，查询共享。删除走墓碑（搜索结果过滤）。
//  持久化：导出/导入平面二进制（magic + dim + metric + 点集 + 图邻接）。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::vector {

enum class Metric : uint8_t {
    Cosine = 0,
    Dot = 1,
    Euclidean = 2,
};

struct HnswConfig {
    int dim = 0;             // 向量维度（必填）
    Metric metric = Metric::Cosine;
    int m = 16;              // 每层最大邻居数（层 0 为 2m）
    int efConstruction = 200;
    int efSearch = 64;       // 查询束宽（≥ k）
    uint64_t seed = 0;       // 0 = 随机
};

struct SearchHit {
    uint64_t id = 0;
    float score = 0;   // 相似度语义：越大越近（Cosine/Dot=相似度；L2=负距离）
    bool operator<(const SearchHit& o) const noexcept { return score > o.score; }
};

class ZENITH_API HnswIndex {
public:
    explicit HnswIndex(HnswConfig cfg);

    // ---- 写 ----
    /// 插入/覆盖一个点（同 id 重复插入 = 覆盖：删旧 + 插新）
    void upsert(uint64_t id, const float* v);
    void remove(uint64_t id);
    /// 批量构建（先全部 upsert 再统一 optimize 无益于 HNSW——逐个插入即可）
    void upsertBulk(const std::vector<std::pair<uint64_t, std::vector<float>>>& points);

    // ---- 读 ----
    [[nodiscard]] std::vector<SearchHit> search(const float* q, int k,
                                                int efSearch = 0) const; // 0 = cfg 默认
    [[nodiscard]] bool get(uint64_t id, std::vector<float>& out) const;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] const HnswConfig& config() const noexcept { return cfg_; }

    // ---- 持久化（平面二进制；little-endian 假设——同机部署场景） ----
    [[nodiscard]] core::Result<std::vector<uint8_t>> serialize() const;
    static core::Result<std::unique_ptr<HnswIndex>> deserialize(const uint8_t* data, size_t n);

private:
    struct Node {
        uint64_t id = 0;
        int level = 0;                                  // 最高层号
        std::vector<float> vec;                          // 已按度量预处理（Cosine 归一化）
        std::vector<std::vector<uint64_t>> links;        // links[layer] → 邻居 id
    };

    // 内部搜索条目（按距离升序的小顶堆 / 结果大顶堆共用）
    struct Cand {
        float dist;
        uint64_t id;
    };

    [[nodiscard]] float dist(const Node& a, const float* q) const;
    [[nodiscard]] float distIds(uint64_t a, uint64_t b) const;
    [[nodiscard]] static float metricApply(Metric m, const float* x, const float* y, int dim);
    [[nodiscard]] int randomLevel();

    // 层内搜索（束宽 ef）；返回按距离升序的候选
    [[nodiscard]] std::vector<Cand> searchLayer(const float* q, const std::vector<Cand>& entry,
                                                int ef, int layer,
                                                const std::unordered_set<uint64_t>& dead) const;
    /// 启发式选邻（离 q 更近且互相不冗余）；keepPruned=true 时用被拒候选补满 M
    [[nodiscard]] std::vector<uint64_t> selectNeighbors(
        const float* q, std::vector<Cand> cands, int m, bool keepPruned) const;

    HnswConfig cfg_;
    float multLevel_;                    // 1/ln(M)
    mutable std::shared_mutex mtx_;      // nodes_/entry_/dead_ 保护
    std::unordered_map<uint64_t, Node> nodes_;
    std::unordered_set<uint64_t> dead_;  // 墓碑（upsert 复用 id 时清除）
    uint64_t entryPoint_ = 0;
    int maxLevel_ = -1;
    std::mt19937_64 rng_;
    std::mutex insertMtx_;               // 串行化插入（图结构修改）
};

} // namespace zenith::vector
