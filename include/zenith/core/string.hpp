// ============================================================================
//  字符串工具（纯头文件实现）
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace zenith::core::string_util {

/// 按 delim 分割；keepEmpty=false 时丢弃空段。
inline std::vector<std::string> split(const std::string& s, char delim,
                                      bool keepEmpty = false) {
    std::vector<std::string> out;
    std::string item;
    for (char c : s) {
        if (c == delim) {
            if (keepEmpty || !item.empty()) out.push_back(item);
            item.clear();
        } else {
            item.push_back(c);
        }
    }
    if (keepEmpty || !item.empty()) out.push_back(item);
    return out;
}

inline std::string trimLeft(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    return s.substr(b);
}

inline std::string trimRight(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(0, e);
}

inline std::string trim(const std::string& s) { return trimLeft(trimRight(s)); }

inline std::string toLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

inline std::string toUpper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

inline bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

} // namespace zenith::core::string_util
