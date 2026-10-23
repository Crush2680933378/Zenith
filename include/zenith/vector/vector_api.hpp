// ============================================================================
//  zenith::vector::mountVectorApi —— Qdrant 兼容 REST 子集（M10.1）
//
//  挂载（宿主持 Router 与服务实例）：
//      vector::mountVectorApi(server.router(), svc);
//
//  端点（Qdrant v1.x 兼容子集）：
//    PUT    /collections                      创建（body: {name,vectors:{size,distance}}）
//    GET    /collections                      列表
//    GET    /collections/:name                详情
//    DELETE /collections/:name                删除
//    PUT    /collections/:name/points         upsert（{points:[{id,vector,payload}]}）
//    POST   /collections/:name/points/search  检索（{vector,limit,score_threshold,with_payload}）
//    POST   /collections/:name/points/delete  删除（{points:[ids]}）
//    GET    /collections/:name/points/:id     取单点
// ============================================================================
#pragma once

#include <memory>
#include <string>

#include "zenith/http/http_server.hpp"
#include "zenith/json/json.hpp"
#include "zenith/vector/vector_service.hpp"

namespace zenith::vector {

namespace detail {

inline const char* metricName(Metric m) {
    switch (m) {
        case Metric::Cosine: return "Cosine";
        case Metric::Dot: return "Dot";
        case Metric::Euclidean: return "Euclidean";
    }
    return "Cosine";
}

inline Metric metricFrom(const std::string& s) {
    if (s == "Dot" || s == "dot") return Metric::Dot;
    if (s == "Euclidean" || s == "euclidean" || s == "L2") return Metric::Euclidean;
    return Metric::Cosine;
}

/// json 数组 → float 向量；尺寸/类型不符返回空
inline std::vector<float> jsonToVec(const json::JsonValue& a, int expectDim) {
    std::vector<float> v;
    const auto* arr = a.asArray();
    if (arr == nullptr || static_cast<int>(arr->size()) != expectDim) return v;
    v.reserve(arr->size());
    for (const json::JsonValue& e : *arr) {
        if (!e.isNumber()) return {};
        v.push_back(static_cast<float>(e.asDouble()));
    }
    return v;
}

inline json::JsonValue okResult(json::JsonValue result) {
    json::JsonValue out = json::JsonValue::object();
    out["result"] = std::move(result);
    out["status"] = "ok";
    out["time"] = 0.0;
    return out;
}

} // namespace detail

inline void mountVectorApi(http::Router& r, std::shared_ptr<IVectorStoreService> svc) {
    using namespace http;

    // ---- 集合管理 ----
    r.put("/collections", [svc](Context& c) {
        auto body = c.bodyJson();
        CollectionConfig cfg;
        cfg.dim = static_cast<int>(body["vectors"]["size"].asInt());
        std::string distName = body["vectors"]["distance"].asString();
        if (distName.empty()) distName = "Cosine";
        cfg.metric = detail::metricFrom(distName);
        if (cfg.dim <= 0) {
            c.status(400).text(400, "400 vectors.size required");
            return;
        }
        auto res = svc->createCollection(body["name"].asString(), cfg);
        if (!res) {
            c.status(400).text(400, "400 " + res.error().message);
            return;
        }
        c.json(200, detail::okResult(true));
    });

    r.get("/collections", [svc](Context& c) {
        json::JsonValue arr = json::JsonValue::array();
        for (const CollectionInfo& ci : svc->collections()) {
            json::JsonValue o = json::JsonValue::object();
            o["name"] = ci.name;
            arr.push_back(std::move(o));
        }
        json::JsonValue result = json::JsonValue::object();
        result["collections"] = std::move(arr);
        c.json(200, detail::okResult(std::move(result)));
    });

    r.get("/collections/:name", [svc](Context& c) {
        auto info = svc->collectionInfo(c.param("name"));
        if (!info) {
            c.status(404).text(404, "404 no such collection");
            return;
        }
        json::JsonValue result = json::JsonValue::object();
        json::JsonValue status = json::JsonValue::object();
        status["status"] = "green";
        status["points_count"] = static_cast<int64_t>(info.value().points);
        result["status"] = std::move(status);
        c.json(200, detail::okResult(std::move(result)));
    });

    r.delete_("/collections/:name", [svc](Context& c) {
        auto res = svc->deleteCollection(c.param("name"));
        c.json(res ? 200 : 404, detail::okResult(static_cast<bool>(res)));
    });

    // ---- 点管理 ----
    r.put("/collections/:name/points", [svc](Context& c) {
        auto body = c.bodyJson();
        const auto* pts = body["points"].asArray();
        if (pts == nullptr) {
            c.status(400).text(400, "400 points[] required");
            return;
        }
        auto info = svc->collectionInfo(c.param("name"));
        if (!info) {
            c.status(404).text(404, "404 no such collection");
            return;
        }
        std::vector<VectorPoint> batch;
        batch.reserve(pts->size());
        for (const json::JsonValue& p : *pts) {
            VectorPoint vp;
            // Qdrant id 可为 int 或 uuid——Zenith v1 仅支持无符号整型
            vp.id = static_cast<uint64_t>(p["id"].asInt());
            if (vp.id == 0) {
                c.status(400).text(400, "400 point id (positive int) required");
                return;
            }
            vp.vec = detail::jsonToVec(p["vector"], info.value().dim);
            if (vp.vec.empty()) {
                c.status(400).text(400, "400 vector size mismatch");
                return;
            }
            if (p["payload"].isObject()) vp.payloadJson = p["payload"].dump();
            batch.push_back(std::move(vp));
        }
        auto res = svc->upsert(c.param("name"), batch);
        if (!res) {
            c.status(500).text(500, "500 " + res.error().message);
            return;
        }
        c.json(200, detail::okResult(true));
    });

    r.post("/collections/:name/points/search", [svc](Context& c) {
        auto body = c.bodyJson();
        auto info = svc->collectionInfo(c.param("name"));
        if (!info) {
            c.status(404).text(404, "404 no such collection");
            return;
        }
        const std::vector<float> q =
            detail::jsonToVec(body["vector"], info.value().dim);
        if (q.empty()) {
            c.status(400).text(400, "400 vector size mismatch");
            return;
        }
        const int limit = static_cast<int>(body["limit"].asInt(10));
        const float thr = static_cast<float>(body["score_threshold"].asDouble(-1e30));
        auto res = svc->search(c.param("name"), q, limit, thr);
        if (!res) {
            c.status(500).text(500, "500 " + res.error().message);
            return;
        }
        json::JsonValue arr = json::JsonValue::array();
        for (const ScoredPoint& sp : res.value()) {
            json::JsonValue o = json::JsonValue::object();
            o["id"] = static_cast<int64_t>(sp.id);
            o["score"] = static_cast<double>(sp.score);
            o["version"] = 1;
            if (!sp.payloadJson.empty() && body["with_payload"].asBool(true)) {
                auto pv = json::JsonValue::parse(sp.payloadJson);
                o["payload"] = pv ? std::move(pv).value() : json::JsonValue::object();
            } else {
                o["payload"] = json::JsonValue();
            }
            arr.push_back(std::move(o));
        }
        c.json(200, detail::okResult(std::move(arr)));
    });

    r.post("/collections/:name/points/delete", [svc](Context& c) {
        auto body = c.bodyJson();
        const auto* ids = body["points"].asArray();
        if (ids == nullptr) {
            c.status(400).text(400, "400 points[] required");
            return;
        }
        std::vector<uint64_t> idList;
        idList.reserve(ids->size());
        for (const json::JsonValue& e : *ids) {
            idList.push_back(static_cast<uint64_t>(e.asInt()));
        }
        auto res = svc->remove(c.param("name"), idList);
        c.json(res ? 200 : 404, detail::okResult(static_cast<bool>(res)));
    });

    r.get("/collections/:name/points/:id", [svc](Context& c) {
        const uint64_t id = std::strtoull(c.param("id").c_str(), nullptr, 10);
        auto res = svc->get(c.param("name"), id);
        if (!res) {
            c.status(404).text(404, "404 no such point");
            return;
        }
        json::JsonValue o = json::JsonValue::object();
        o["id"] = static_cast<int64_t>(res.value().id);
        json::JsonValue vec = json::JsonValue::array();
        for (float x : res.value().vec) vec.push_back(static_cast<double>(x));
        o["vector"] = std::move(vec);
        if (!res.value().payloadJson.empty()) {
            auto pv = json::JsonValue::parse(res.value().payloadJson);
            o["payload"] = pv ? std::move(pv).value() : json::JsonValue::object();
        }
        c.json(200, detail::okResult(std::move(o)));
    });
}

} // namespace zenith::vector
