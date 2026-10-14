// ============================================================================
//  zenith::json —— 自研 JSON 值模型 / 递归下降解析器 / 序列化器（零三方依赖）
//  - 解析严格遵守 RFC 8259（UTF-8、\uXXXX 含代理对、深度限制 128）
//  - 数值：无小数/指数部分且在 int64 范围内 → Int，否则 Double
//  - 对象按键字典序存储（dump 输出确定序）
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::json {

class ZENITH_API JsonValue {
public:
    enum class Type : uint8_t {
        Null = 0,
        Bool,
        Int,
        Double,
        String,
        Array,
        Object,
    };

    using Array  = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>; // 字典序 → 输出确定

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool b) : type_(Type::Bool), b_(b) {}
    JsonValue(int v) : type_(Type::Int), i_(v) {}
    JsonValue(int64_t v) : type_(Type::Int), i_(v) {}
    JsonValue(uint32_t v) : type_(Type::Int), i_(static_cast<int64_t>(v)) {}
    JsonValue(double d) : type_(Type::Double), d_(d) {}
    JsonValue(const char* s) : type_(Type::String), s_(s) {}
    JsonValue(std::string s) : type_(Type::String), s_(std::move(s)) {}

    static JsonValue array() { return JsonValue(Type::Array); }
    static JsonValue object() { return JsonValue(Type::Object); }

    /* ---- 类型判断 ---- */
    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isInt() const noexcept { return type_ == Type::Int; }
    [[nodiscard]] bool isDouble() const noexcept { return type_ == Type::Double; }
    [[nodiscard]] bool isNumber() const noexcept { return isInt() || isDouble(); }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    /* ---- 标量读取（带默认值，永不抛出） ---- */
    [[nodiscard]] bool asBool(bool def = false) const noexcept {
        return type_ == Type::Bool ? b_ : def;
    }
    [[nodiscard]] int64_t asInt(int64_t def = 0) const noexcept {
        if (type_ == Type::Int) return i_;
        if (type_ == Type::Double) return static_cast<int64_t>(d_);
        return def;
    }
    [[nodiscard]] double asDouble(double def = 0.0) const noexcept {
        if (type_ == Type::Double) return d_;
        if (type_ == Type::Int) return static_cast<double>(i_);
        return def;
    }
    /// 仅 String 时返回其值；否则返回空串。
    [[nodiscard]] const std::string& asString() const noexcept;

    /* ---- object 访问 ---- */
    /// 写路径：非 object 自动转换；键缺失则插入 Null 并返回其引用。
    JsonValue& operator[](const std::string& key);
    /// 读路径：键不存在返回 nullptr。
    [[nodiscard]] const JsonValue* find(const std::string& key) const;
    /// 读路径便捷：键不存在返回静态 null（只读场景）。
    [[nodiscard]] const JsonValue& operator[](const std::string& key) const noexcept {
        if (const JsonValue* p = find(key)) return *p;
        static const JsonValue kNull;
        return kNull;
    }
    [[nodiscard]] bool has(const std::string& key) const { return find(key) != nullptr; }
    void set(const std::string& key, JsonValue v) { (*this)[key] = std::move(v); }
    [[nodiscard]] size_t erase(const std::string& key);
    [[nodiscard]] const Object* asObject() const noexcept {
        return type_ == Type::Object ? &obj_ : nullptr;
    }

    /* ---- array 访问 ---- */
    void push_back(JsonValue v);
    [[nodiscard]] const JsonValue& at(size_t i) const;      // 越界抛 out_of_range
    [[nodiscard]] JsonValue& at(size_t i);
    [[nodiscard]] const JsonValue& operator[](size_t i) const noexcept; // 越界返回静态 null
    [[nodiscard]] const Array* asArray() const noexcept {
        return type_ == Type::Array ? &arr_ : nullptr;
    }

    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    /* ---- 解析 / 序列化 ---- */
    /// 解析 JSON 文本；失败返回 Err（含错误位置描述）。
    static core::Result<JsonValue> parse(const std::string& text);
    /// indent < 0 紧凑输出；>= 0 按该缩进美化（换行 + 缩进）。
    [[nodiscard]] std::string dump(int indent = -1) const;

private:
    explicit JsonValue(Type t) : type_(t) {}

    Type        type_ = Type::Null;
    bool        b_    = false;
    int64_t     i_    = 0;
    double      d_    = 0.0;
    std::string s_;
    Array       arr_;
    Object      obj_;
};

} // namespace zenith::json
