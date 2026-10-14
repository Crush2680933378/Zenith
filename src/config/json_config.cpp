#include "zenith/config/json_config.hpp"

#include <fstream>

namespace zenith::config {

core::Result<JsonConfig> JsonConfig::loadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return core::Result<JsonConfig>::Err(
            core::Error(70, "JsonConfig: cannot open file: " + path));
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return fromString(text);
}

core::Result<JsonConfig> JsonConfig::fromString(const std::string& text) {
    auto v = json::JsonValue::parse(text);
    if (!v) {
        return core::Result<JsonConfig>::Err(std::move(v).error());
    }
    if (!v.value().isObject()) {
        return core::Result<JsonConfig>::Err(
            core::Error(71, "JsonConfig: root must be an object"));
    }
    return core::Result<JsonConfig>::Ok(JsonConfig(std::move(v).value()));
}

std::string JsonConfig::get(const std::string& dottedKey, const std::string& def) const {
    const json::JsonValue* cur = &root_;
    size_t begin = 0;
    while (begin <= dottedKey.size()) {
        const size_t dot = dottedKey.find('.', begin);
        const std::string seg =
            dottedKey.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin);
        if (seg.empty()) return def;
        if (cur->isObject()) {
            cur = cur->find(seg);
        } else if (cur->isArray()) {
            char* endp = nullptr;
            const long idx = std::strtol(seg.c_str(), &endp, 10);
            if (endp == seg.c_str() || *endp != '\0') return def;
            const auto* arr = cur->asArray();
            if (arr == nullptr || idx < 0 ||
                static_cast<size_t>(idx) >= arr->size()) {
                return def;
            }
            cur = &(*arr)[static_cast<size_t>(idx)];
        } else {
            return def;
        }
        if (cur == nullptr) return def;
        if (dot == std::string::npos) break;
        begin = dot + 1;
    }
    if (cur == nullptr) return def;

    switch (cur->type()) {
        case json::JsonValue::Type::String:
            return cur->asString();
        case json::JsonValue::Type::Int:
            return std::to_string(cur->asInt());
        case json::JsonValue::Type::Double:
            return cur->dump(); // 最短往返形态
        case json::JsonValue::Type::Bool:
            return cur->asBool() ? "true" : "false";
        default:
            return def; // null/object/array → 默认
    }
}

json::JsonValue JsonConfig::section(const std::string& dottedKey) const {
    const json::JsonValue* cur = &root_;
    size_t begin = 0;
    while (begin <= dottedKey.size() && cur != nullptr) {
        const size_t dot = dottedKey.find('.', begin);
        const std::string seg =
            dottedKey.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin);
        if (!cur->isObject()) return json::JsonValue();
        cur = seg.empty() ? nullptr : cur->find(seg);
        if (dot == std::string::npos) break;
        begin = dot + 1;
    }
    return cur != nullptr ? *cur : json::JsonValue();
}

void JsonConfig::flattenInto(const json::JsonValue& node, const std::string& prefix,
                              core::KeyValueConfig& out) {
    if (node.isObject()) {
        if (const auto* obj = node.asObject()) {
            for (const auto& [k, v] : *obj) {
                flattenInto(v, prefix.empty() ? k : prefix + "." + k, out);
            }
        }
        return;
    }
    if (node.isArray()) {
        if (const auto* arr = node.asArray()) {
            for (size_t i = 0; i < arr->size(); ++i) {
                flattenInto((*arr)[i], prefix + "." + std::to_string(i), out);
            }
        }
        return;
    }
    // 标量
    std::string value;
    switch (node.type()) {
        case json::JsonValue::Type::String: value = node.asString(); break;
        case json::JsonValue::Type::Int:     value = std::to_string(node.asInt()); break;
        case json::JsonValue::Type::Double:  value = node.dump(); break;
        case json::JsonValue::Type::Bool:    value = node.asBool() ? "true" : "false"; break;
        default: value = "null"; break;
    }
    out.set(prefix, value);
}

core::KeyValueConfig JsonConfig::flatten() const {
    core::KeyValueConfig out;
    flattenInto(root_, "", out);
    return out;
}

} // namespace zenith::config
