#include "zenith/vector/hnsw.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>

namespace zenith::vector {

namespace {
constexpr uint32_t kMagic = 0x5A484E57; // "ZHNS"
} // namespace

HnswIndex::HnswIndex(HnswConfig cfg) : cfg_(cfg) {
    if (cfg_.m < 2) cfg_.m = 2;
    if (cfg_.efConstruction < cfg_.m) cfg_.efConstruction = cfg_.m;
    if (cfg_.efSearch < 1) cfg_.efSearch = 1;
    multLevel_ = 1.0f / std::log(static_cast<float>(cfg_.m));
    rng_.seed(cfg_.seed != 0 ? cfg_.seed
                             : (std::random_device{}() ^ static_cast<uint64_t>(__TIME__[0])));
}

float HnswIndex::metricApply(Metric m, const float* x, const float* y, int dim) {
    float dot = 0;
    switch (m) {
        case Metric::Cosine:
        case Metric::Dot:
            for (int i = 0; i < dim; ++i) dot += x[i] * y[i];
            return m == Metric::Cosine ? 1.0f - dot : -dot;
        case Metric::Euclidean: {
            float acc = 0;
            for (int i = 0; i < dim; ++i) {
                const float d = x[i] - y[i];
                acc += d * d;
            }
            return std::sqrt(acc);
        }
    }
    return 0;
}

float HnswIndex::dist(const Node& a, const float* q) const {
    return metricApply(cfg_.metric, a.vec.data(), q, cfg_.dim);
}

float HnswIndex::distIds(uint64_t a, uint64_t b) const {
    // 调用方持锁
    auto ia = nodes_.find(a);
    auto ib = nodes_.find(b);
    if (ia == nodes_.end() || ib == nodes_.end()) return 1e30f;
    return metricApply(cfg_.metric, ia->second.vec.data(), ib->second.vec.data(), cfg_.dim);
}

int HnswIndex::randomLevel() {
    // 指数分布：P(level ≥ l) = m^-l
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    int level = 0;
    const float r = u(rng_);
    // level = floor(-ln(r) * multLevel)，逐步判定防爆
    float f = r;
    while (f < 1.0f && level < 32) {
        ++level;
        f *= static_cast<float>(cfg_.m);
    }
    return level - 1 < 0 ? 0 : level - 1;
}

std::vector<HnswIndex::Cand> HnswIndex::searchLayer(
    const float* q, const std::vector<Cand>& entry, int ef, int layer,
    const std::unordered_set<uint64_t>& dead) const {
    // frontier：堆顶 = 最近候选（greater 比较器 → top 为最小 dist，先扩展最近侧）
    // best：堆顶 = 当前 ef 名中最远（less 比较器 → top 为最大 dist，供剪枝与淘汰）
    //   注：priority_queue 的 top 是比较器意义下的"最大"——两比较器职责不可颠倒
    auto nearer = [](const Cand& a, const Cand& b) { return a.dist > b.dist; };
    auto farther = [](const Cand& a, const Cand& b) { return a.dist < b.dist; };

    std::priority_queue<Cand, std::vector<Cand>, decltype(nearer)> frontier(nearer);
    std::priority_queue<Cand, std::vector<Cand>, decltype(farther)> best(farther);
    std::unordered_set<uint64_t> visited;

    for (const Cand& e : entry) {
        if (visited.insert(e.id).second) frontier.push(e);
    }
    // 初始 best = 入口点（含墓碑——以距离论，后续统一过滤）
    for (const Cand& e : entry) best.push(e);

    while (!frontier.empty()) {
        const Cand c = frontier.top();
        frontier.pop();
        // 剪枝：最近候选比当前第 ef 名还远 → 结束
        if (!best.empty() && static_cast<int>(best.size()) >= ef &&
            c.dist > best.top().dist) {
            break;
        }
        auto it = nodes_.find(c.id);
        if (it == nodes_.end() || layer >= static_cast<int>(it->second.links.size())) continue;
        for (uint64_t nb : it->second.links[static_cast<size_t>(layer)]) {
            if (!visited.insert(nb).second) continue;
            auto nit = nodes_.find(nb);
            if (nit == nodes_.end()) continue;
            const float d = dist(nit->second, q);
            if (static_cast<int>(best.size()) < ef || d < best.top().dist) {
                frontier.push(Cand{d, nb});
                best.push(Cand{d, nb});
                if (static_cast<int>(best.size()) > ef) best.pop();
            }
        }
    }

    std::vector<Cand> out;
    out.reserve(best.size());
    while (!best.empty()) {
        if (dead.count(best.top().id) == 0) out.push_back(best.top());
        best.pop();
    }
    std::sort(out.begin(), out.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    return out;
}

std::vector<uint64_t> HnswIndex::selectNeighbors(const float* /*q*/, std::vector<Cand> cands,
                                                 int m, bool keepPruned) const {
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    std::vector<uint64_t> selected;
    std::vector<Cand> discarded;
    for (const Cand& c : cands) {
        if (static_cast<int>(selected.size()) >= m) break;
        bool ok = true;
        for (uint64_t s : selected) {
            // 启发式：候选若比离 q 更近的已选邻居之间更近 → 冗余，拒绝
            if (distIds(c.id, s) < c.dist) {
                ok = false;
                break;
            }
        }
        if (ok) {
            selected.push_back(c.id);
        } else {
            discarded.push_back(c);
        }
    }
    if (keepPruned) {
        for (const Cand& c : discarded) {
            if (static_cast<int>(selected.size()) >= m) break;
            selected.push_back(c.id);
        }
    }
    return selected;
}

void HnswIndex::upsert(uint64_t id, const float* v) {
    std::lock_guard<std::mutex> inslk(insertMtx_);
    std::unique_lock<std::shared_mutex> lk(mtx_);

    // 预处理向量（Cosine → 归一化；其余原样）
    Node node;
    node.id = id;
    node.vec.assign(v, v + cfg_.dim);
    if (cfg_.metric == Metric::Cosine) {
        float norm = 0;
        for (float x : node.vec) norm += x * x;
        norm = std::sqrt(norm);
        if (norm > 0) {
            for (float& x : node.vec) x /= norm;
        }
    }

    // 覆盖语义：先删旧
    if (nodes_.count(id) > 0) {
        dead_.erase(id);
        // 旧点的连边留在邻居表里，查询时经 nodes_.find 跳过——
        // 换 id 不变则邻接重建为下述流程（先物理移除旧节点）
        nodes_.erase(id);
        // 清理他点指向旧 id 的连边（保守遍历代价可接受：插入路径非热查询路径）
        for (auto& [nid, n] : nodes_) {
            for (auto& layer : n.links) {
                layer.erase(std::remove(layer.begin(), layer.end(), id), layer.end());
            }
        }
        // 入口若被删 → 重新指定
        if (entryPoint_ == id) {
            if (!nodes_.empty()) {
                entryPoint_ = nodes_.begin()->first;
                maxLevel_ = nodes_.begin()->second.level;
            } else {
                entryPoint_ = 0;
                maxLevel_ = -1;
            }
        }
    }

    const int level = randomLevel();
    node.level = level;

    if (nodes_.empty()) {
        node.links.assign(static_cast<size_t>(level) + 1, {});
        nodes_.emplace(id, std::move(node));
        entryPoint_ = id;
        maxLevel_ = level;
        return;
    }

    // 1) 自顶向下贪心（ef=1）定位至 level+1 层（此时新点尚未入图，不会自搜）
    std::vector<float> qv = node.vec; // 拷贝——node 后续 move 入图
    std::vector<Cand> ep{Cand{dist(nodes_.at(entryPoint_), qv.data()), entryPoint_}};
    for (int l = maxLevel_; l > level; --l) {
        auto r = searchLayer(qv.data(), ep, 1, l, dead_);
        if (!r.empty()) ep = {r.front()};
    }

    // 2) 逐层选邻（新点仍未入图）
    std::vector<std::vector<uint64_t>> layerNeighbors(static_cast<size_t>(level) + 1);
    for (int l = std::min(level, maxLevel_); l >= 0; --l) {
        auto cands = searchLayer(qv.data(), ep, cfg_.efConstruction, l, dead_);
        layerNeighbors[static_cast<size_t>(l)] =
            selectNeighbors(qv.data(), std::move(cands), cfg_.m, true);
        // 下一层入口 = 本层最优
        // （cands 已 move——重新以首邻居为入口即可；保底用当前 ep）
        if (!layerNeighbors[static_cast<size_t>(l)].empty()) {
            auto nit = nodes_.find(layerNeighbors[static_cast<size_t>(l)].front());
            if (nit != nodes_.end()) {
                ep = {Cand{dist(nit->second, qv.data()), nit->first}};
            }
        }
    }

    // 3) 一次性入图
    node.links = layerNeighbors;
    nodes_.emplace(id, std::move(node));

    // 4) 反向连边 + 剪枝
    for (int l = std::min(level, maxLevel_); l >= 0; --l) {
        const int mMax = l == 0 ? cfg_.m * 2 : cfg_.m;
        for (uint64_t nb : layerNeighbors[static_cast<size_t>(l)]) {
            auto nit = nodes_.find(nb);
            if (nit == nodes_.end()) continue;
            auto& layer = nit->second.links[static_cast<size_t>(l)];
            layer.push_back(id);
            if (static_cast<int>(layer.size()) > mMax) {
                // 超限 → 以 nb 为中心重选 mMax 个
                std::vector<Cand> all;
                all.reserve(layer.size());
                for (uint64_t cand : layer) {
                    auto cit = nodes_.find(cand);
                    if (cit != nodes_.end()) {
                        all.push_back(
                            Cand{metricApply(cfg_.metric, nit->second.vec.data(),
                                             cit->second.vec.data(), cfg_.dim),
                                 cand});
                    }
                }
                layer = selectNeighbors(nit->second.vec.data(), std::move(all), mMax, true);
            }
        }
    }

    if (level > maxLevel_) {
        maxLevel_ = level;
        entryPoint_ = id;
    }
}

void HnswIndex::upsertBulk(const std::vector<std::pair<uint64_t, std::vector<float>>>& points) {
    for (const auto& [id, v] : points) {
        if (static_cast<int>(v.size()) == cfg_.dim) upsert(id, v.data());
    }
}

void HnswIndex::remove(uint64_t id) {
    std::lock_guard<std::mutex> inslk(insertMtx_);
    std::unique_lock<std::shared_mutex> lk(mtx_);
    if (nodes_.count(id) > 0) {
        dead_.insert(id);
    }
}

bool HnswIndex::get(uint64_t id, std::vector<float>& out) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    auto it = nodes_.find(id);
    if (it == nodes_.end() || dead_.count(id) > 0) return false;
    out = it->second.vec;
    if (cfg_.metric == Metric::Cosine) {
        // 归一化向量无法还原原始方向模长——返回归一化形式（检索场景够用）
    }
    return true;
}

size_t HnswIndex::size() const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return nodes_.size() - dead_.size();
}

std::vector<SearchHit> HnswIndex::search(const float* q, int k, int efSearch) const {
    const int ef = efSearch > 0 ? std::max(efSearch, k) : std::max(cfg_.efSearch, k);
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (nodes_.empty() || maxLevel_ < 0) return {};

    // 查询向量同样按度量预处理
    std::vector<float> qv(q, q + cfg_.dim);
    if (cfg_.metric == Metric::Cosine) {
        float norm = 0;
        for (float x : qv) norm += x * x;
        norm = std::sqrt(norm);
        if (norm > 0) {
            for (float& x : qv) x /= norm;
        }
    }

    // 自顶向下贪心
    std::vector<Cand> ep{Cand{dist(nodes_.at(entryPoint_), qv.data()), entryPoint_}};
    for (int l = maxLevel_; l >= 1; --l) {
        auto r = searchLayer(qv.data(), ep, 1, l, dead_);
        if (!r.empty()) ep = {r.front()};
    }
    auto cands = searchLayer(qv.data(), ep, ef, 0, dead_);

    // 距离 → 相似度语义（越大越好；与 Qdrant 对齐）
    std::vector<SearchHit> out;
    out.reserve(static_cast<size_t>(k));
    for (const Cand& c : cands) {
        if (static_cast<int>(out.size()) >= k) break;
        SearchHit h;
        h.id = c.id;
        h.score = cfg_.metric == Metric::Cosine ? 1.0f - c.dist  // = dot
                                                : -c.dist;       // Dot: -(-dot)；L2: -距离
        out.push_back(h);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 序列化：| magic u32 | dim i32 | metric u8 | m i32 | efc i32 | efs i32 |
//         | count u32 | 每点: id u64 | level u32 | vec f32×dim |
//         | 每层: deg u32 | ids u64×deg | | dead 数 u32 | dead ids |
// ---------------------------------------------------------------------------
core::Result<std::vector<uint8_t>> HnswIndex::serialize() const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    std::vector<uint8_t> buf;
    auto put = [&buf](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    };
    put(&kMagic, 4);
    put(&cfg_.dim, 4);
    put(&cfg_.metric, 1);
    put(&cfg_.m, 4);
    put(&cfg_.efConstruction, 4);
    put(&cfg_.efSearch, 4);
    const uint32_t count = static_cast<uint32_t>(nodes_.size());
    put(&count, 4);
    for (const auto& [id, n] : nodes_) {
        put(&id, 8);
        const uint32_t lvl = static_cast<uint32_t>(n.level);
        put(&lvl, 4);
        put(n.vec.data(), static_cast<size_t>(cfg_.dim) * 4);
        for (const auto& layer : n.links) {
            const uint32_t deg = static_cast<uint32_t>(layer.size());
            put(&deg, 4);
            if (!layer.empty()) put(layer.data(), layer.size() * 8);
        }
    }
    const uint32_t deadCount = static_cast<uint32_t>(dead_.size());
    put(&deadCount, 4);
    for (uint64_t d : dead_) put(&d, 8);
    return core::Result<std::vector<uint8_t>>::Ok(std::move(buf));
}

core::Result<std::unique_ptr<HnswIndex>> HnswIndex::deserialize(const uint8_t* data, size_t n) {
    if (n < 29) {
        return core::Result<std::unique_ptr<HnswIndex>>::Err(
            core::Error(200, "hnsw: truncated blob"));
    }
    size_t pos = 0;
    auto take = [&](void* dst, size_t sz) -> bool {
        if (pos + sz > n) return false;
        std::memcpy(dst, data + pos, sz);
        pos += sz;
        return true;
    };
    uint32_t magic = 0;
    if (!take(&magic, 4) || magic != kMagic) {
        return core::Result<std::unique_ptr<HnswIndex>>::Err(core::Error(200, "hnsw: bad magic"));
    }
    HnswConfig cfg;
    if (!take(&cfg.dim, 4) || !take(&cfg.metric, 1) || !take(&cfg.m, 4) ||
        !take(&cfg.efConstruction, 4) || !take(&cfg.efSearch, 4)) {
        return core::Result<std::unique_ptr<HnswIndex>>::Err(core::Error(200, "hnsw: truncated header"));
    }
    auto idxp = std::make_unique<HnswIndex>(cfg);
    HnswIndex& idx = *idxp;
    std::unique_lock<std::shared_mutex> lk(idx.mtx_);
    uint32_t count = 0;
    if (!take(&count, 4)) {
        return core::Result<std::unique_ptr<HnswIndex>>::Err(core::Error(200, "hnsw: truncated count"));
    }
    for (uint32_t i = 0; i < count; ++i) {
        Node node;
        uint32_t lvl = 0;
        if (!take(&node.id, 8) || !take(&lvl, 4)) {
            return core::Result<std::unique_ptr<HnswIndex>>::Err(core::Error(200, "hnsw: truncated point"));
        }
        node.level = static_cast<int>(lvl);
        node.vec.resize(static_cast<size_t>(cfg.dim));
        if (!take(node.vec.data(), static_cast<size_t>(cfg.dim) * 4)) {
            return core::Result<std::unique_ptr<HnswIndex>>::Err(core::Error(200, "hnsw: truncated vector"));
        }
        node.links.resize(static_cast<size_t>(lvl) + 1);
        for (auto& layer : node.links) {
            uint32_t deg = 0;
            if (!take(&deg, 4)) {
                return core::Result<std::unique_ptr<HnswIndex>>::Err(core::Error(200, "hnsw: truncated deg"));
            }
            layer.resize(deg);
            if (deg > 0 && !take(layer.data(), static_cast<size_t>(deg) * 8)) {
                return core::Result<std::unique_ptr<HnswIndex>>::Err(core::Error(200, "hnsw: truncated links"));
            }
        }
        if (node.level > idx.maxLevel_) {
            idx.maxLevel_ = node.level;
            idx.entryPoint_ = node.id;
        }
        idx.nodes_.emplace(node.id, std::move(node));
    }
    uint32_t deadCount = 0;
    if (!take(&deadCount, 4)) {
        return core::Result<std::unique_ptr<HnswIndex>>::Err(core::Error(200, "hnsw: truncated dead count"));
    }
    for (uint32_t i = 0; i < deadCount; ++i) {
        uint64_t d = 0;
        if (!take(&d, 8)) {
            return core::Result<std::unique_ptr<HnswIndex>>::Err(core::Error(200, "hnsw: truncated dead id"));
        }
        idx.dead_.insert(d);
    }
    lk.unlock();
    return core::Result<std::unique_ptr<HnswIndex>>::Ok(std::move(idxp));
}

} // namespace zenith::vector
