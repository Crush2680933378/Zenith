#include "zenith/json/json.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace zenith::json {

namespace {

// ---------------------------------------------------------------------------
// 解析器：递归下降，游标 + 深度限制
// ---------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    core::Result<JsonValue> run() {
        skipWs();
        core::Result<JsonValue> v = parseValue(0);
        if (!v) return v;
        skipWs();
        if (pos_ != s_.size()) return err("trailing characters after JSON value");
        return v;
    }

private:
    static constexpr int kMaxDepth = 128;

    core::Result<JsonValue> err(const std::string& msg) const {
        return core::Result<JsonValue>::Err(
            core::Error(1, "json parse error at offset " + std::to_string(pos_) + ": " + msg));
    }

    void skipWs() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool matchKeyword(const char* kw) {
        const size_t n = std::strlen(kw);
        if (s_.compare(pos_, n, kw) == 0) {
            pos_ += n;
            return true;
        }
        return false;
    }

    core::Result<JsonValue> parseValue(int depth) {
        if (depth > kMaxDepth) return err("nesting too deep (> " + std::to_string(kMaxDepth) + ")");
        skipWs();
        if (pos_ >= s_.size()) return err("unexpected end of input");
        const char c = s_[pos_];
        switch (c) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': {
                auto r = parseString();
                if (!r) return core::Result<JsonValue>::Err(std::move(r).error());
                return core::Result<JsonValue>::Ok(JsonValue(std::move(r).value()));
            }
            case 't':
                if (matchKeyword("true")) return core::Result<JsonValue>::Ok(JsonValue(true));
                return err("invalid literal");
            case 'f':
                if (matchKeyword("false")) return core::Result<JsonValue>::Ok(JsonValue(false));
                return err("invalid literal");
            case 'n':
                if (matchKeyword("null")) return core::Result<JsonValue>::Ok(JsonValue(nullptr));
                return err("invalid literal");
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
                return err(std::string("unexpected character '") + c + "'");
        }
    }

    core::Result<JsonValue> parseObject(int depth) {
        ++pos_; // '{'
        JsonValue v = JsonValue::object();
        skipWs();
        if (consume('}')) return core::Result<JsonValue>::Ok(std::move(v));

        for (;;) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return err("expected object key string");
            auto key = parseString();
            if (!key) return core::Result<JsonValue>::Err(std::move(key).error());
            skipWs();
            if (!consume(':')) return err("expected ':' after object key");
            auto val = parseValue(depth + 1);
            if (!val) return val;
            v.set(std::move(key).value(), std::move(val).value());

            skipWs();
            if (consume(',')) continue;
            if (consume('}')) return core::Result<JsonValue>::Ok(std::move(v));
            return err("expected ',' or '}' in object");
        }
    }

    core::Result<JsonValue> parseArray(int depth) {
        ++pos_; // '['
        JsonValue v = JsonValue::array();
        skipWs();
        if (consume(']')) return core::Result<JsonValue>::Ok(std::move(v));

        for (;;) {
            auto val = parseValue(depth + 1);
            if (!val) return val;
            v.push_back(std::move(val).value());
            skipWs();
            if (consume(',')) continue;
            if (consume(']')) return core::Result<JsonValue>::Ok(std::move(v));
            return err("expected ',' or ']' in array");
        }
    }

    core::Result<std::string> parseString() {
        ++pos_; // '"'
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) return strErr("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return core::Result<std::string>::Ok(std::move(out));
            }
            if (c < 0x20) return strErr("raw control character in string");
            if (c == '\\') {
                ++pos_;
                if (pos_ >= s_.size()) return strErr("unterminated escape");
                const char e = s_[pos_++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        auto cp = parseHex4();
                        if (!cp) return core::Result<std::string>::Err(std::move(cp).error());
                        uint32_t code = cp.value();
                        if (code >= 0xD800 && code <= 0xDBFF) { // 高代理 → 需要低代理
                            if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                                pos_ += 2;
                                auto lo = parseHex4();
                                if (!lo) return core::Result<std::string>::Err(std::move(lo).error());
                                const uint32_t loV = lo.value();
                                if (loV >= 0xDC00 && loV <= 0xDFFF) {
                                    code = 0x10000 + ((code - 0xD800) << 10) + (loV - 0xDC00);
                                } else {
                                    return strErr("invalid low surrogate");
                                }
                            } else {
                                return strErr("lone high surrogate");
                            }
                        } else if (code >= 0xDC00 && code <= 0xDFFF) {
                            return strErr("lone low surrogate");
                        }
                        appendUtf8(out, code);
                        break;
                    }
                    default: return strErr(std::string("invalid escape '\\") + e + "'");
                }
            } else {
                out.push_back(static_cast<char>(c));
                ++pos_;
            }
        }
    }

    core::Result<uint32_t> parseHex4() {
        if (pos_ + 4 > s_.size()) return hexErr("truncated \\uXXXX");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9') {
                v |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                v |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                v |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
                return hexErr("invalid hex digit in \\uXXXX");
            }
        }
        return core::Result<uint32_t>::Ok(v);
    }

    core::Result<JsonValue> parseNumber() {
        const size_t begin = pos_;
        consume('-');
        // 整数部分：0 或 [1-9][0-9]*
        if (pos_ >= s_.size()) return err("truncated number");
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            return err("invalid number");
        }
        bool isDouble = false;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            isDouble = true;
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') {
                return err("digit required after decimal point");
            }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            isDouble = true;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') {
                return err("digit required in exponent");
            }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }

        const std::string numStr = s_.substr(begin, pos_ - begin);
        if (!isDouble) {
            int64_t iv = 0;
            const auto [ptr, ec] = std::from_chars(numStr.data(), numStr.data() + numStr.size(), iv);
            if (ec == std::errc() && ptr == numStr.data() + numStr.size()) {
                return core::Result<JsonValue>::Ok(JsonValue(iv));
            }
            // 溢出 int64 → 按双精度处理
        }
        errno = 0;
        double dv = 0;
        const auto [ptr2, ec2] = std::from_chars(numStr.data(), numStr.data() + numStr.size(), dv);
        if (ec2 != std::errc()) return err("number out of range");
        (void)ptr2;
        return core::Result<JsonValue>::Ok(JsonValue(dv));
    }

    core::Result<std::string> strErr(const std::string& msg) const {
        return core::Result<std::string>::Err(
            core::Error(2, "json parse error at offset " + std::to_string(pos_) + ": " + msg));
    }
    core::Result<uint32_t> hexErr(const std::string& msg) const {
        return core::Result<uint32_t>::Err(
            core::Error(3, "json parse error at offset " + std::to_string(pos_) + ": " + msg));
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// 序列化器
// ---------------------------------------------------------------------------
void dumpEscaped(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void dumpImpl(const JsonValue& v, std::string& out, int indent, int depth) {
    const auto pad = [&](int d) {
        if (indent >= 0) {
            out.push_back('\n');
            out.append(static_cast<size_t>(indent) * static_cast<size_t>(d), ' ');
        }
    };

    switch (v.type()) {
        case JsonValue::Type::Null:    out += "null"; break;
        case JsonValue::Type::Bool:    out += v.asBool() ? "true" : "false"; break;
        case JsonValue::Type::Int:     out += std::to_string(v.asInt()); break;
        case JsonValue::Type::Double: {
            const double d = v.asDouble();
            if (std::isnan(d) || std::isinf(d)) {
                out += "null"; // JSON 无 NaN/Inf 表示
                break;
            }
            char buf[64];
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), d); // 最短往返表示
            if (ec == std::errc()) {
                out.append(buf, static_cast<size_t>(ptr - buf));
                // 保证是合法 JSON 数值（如 "1e5" → "1e5" 合法；整数形如 "1" 补 ".0"）
                const std::string seg(buf, static_cast<size_t>(ptr - buf));
                if (seg.find('.') == std::string::npos && seg.find('e') == std::string::npos &&
                    seg.find('E') == std::string::npos) {
                    out += ".0";
                }
            } else {
                out += "null";
            }
            break;
        }
        case JsonValue::Type::String:
            dumpEscaped(v.asString(), out);
            break;
        case JsonValue::Type::Array: {
            const auto* arr = v.asArray();
            if (arr->empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            bool first = true;
            for (const JsonValue& e : *arr) {
                if (!first) out.push_back(',');
                first = false;
                pad(depth + 1);
                dumpImpl(e, out, indent, depth + 1);
            }
            pad(depth);
            out.push_back(']');
            break;
        }
        case JsonValue::Type::Object: {
            const auto* obj = v.asObject();
            if (obj->empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            bool first = true;
            for (const auto& [k, e] : *obj) {
                if (!first) out.push_back(',');
                first = false;
                pad(depth + 1);
                dumpEscaped(k, out);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                dumpImpl(e, out, indent, depth + 1);
            }
            pad(depth);
            out.push_back('}');
            break;
        }
    }
}

const JsonValue& staticNull() {
    static const JsonValue kNull;
    return kNull;
}

} // namespace

const std::string& JsonValue::asString() const noexcept {
    return type_ == Type::String ? s_ : staticNull().s_;
}

JsonValue& JsonValue::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        obj_.clear();
    }
    return obj_[key];
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    auto it = obj_.find(key);
    return it == obj_.end() ? nullptr : &it->second;
}

size_t JsonValue::erase(const std::string& key) {
    if (type_ != Type::Object) return 0;
    return obj_.erase(key);
}

void JsonValue::push_back(JsonValue v) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        arr_.clear();
    }
    arr_.push_back(std::move(v));
}

const JsonValue& JsonValue::at(size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) {
        throw std::out_of_range("JsonValue::at: index out of range");
    }
    return arr_[i];
}

JsonValue& JsonValue::at(size_t i) {
    if (type_ != Type::Array || i >= arr_.size()) {
        throw std::out_of_range("JsonValue::at: index out of range");
    }
    return arr_[i];
}

const JsonValue& JsonValue::operator[](size_t i) const noexcept {
    if (type_ != Type::Array || i >= arr_.size()) return staticNull();
    return arr_[i];
}

size_t JsonValue::size() const noexcept {
    switch (type_) {
        case Type::Array:  return arr_.size();
        case Type::Object: return obj_.size();
        default:           return 0;
    }
}

bool JsonValue::empty() const noexcept {
    switch (type_) {
        case Type::Array:  return arr_.empty();
        case Type::Object: return obj_.empty();
        case Type::String: return s_.empty();
        case Type::Null:   return true;
        default:           return false;
    }
}

core::Result<JsonValue> JsonValue::parse(const std::string& text) {
    return Parser(text).run();
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    out.reserve(64);
    dumpImpl(*this, out, indent, 0);
    return out;
}

} // namespace zenith::json
