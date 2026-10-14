#include "zenith/core/key_value_config.hpp"

#include <fstream>

#include "zenith/core/string.hpp"

namespace zenith::core {

using string_util::split;
using string_util::toLower;
using string_util::trim;

void KeyValueConfig::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mtx_);
    map_[toLower(trim(key))] = value;
}

std::string KeyValueConfig::get(const std::string& key, const std::string& defaultValue) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(toLower(trim(key)));
    return it == map_.end() ? defaultValue : it->second;
}

bool KeyValueConfig::tryGet(const std::string& key, std::string& outValue) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = map_.find(toLower(trim(key)));
    if (it == map_.end()) return false;
    outValue = it->second;
    return true;
}

bool KeyValueConfig::contains(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return map_.find(toLower(trim(key))) != map_.end();
}

size_t KeyValueConfig::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return map_.size();
}

void KeyValueConfig::remove(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    map_.erase(toLower(trim(key)));
}

void KeyValueConfig::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    map_.clear();
}

size_t KeyValueConfig::parseFromString(const std::string& text) {
    size_t count = 0;
    for (const std::string& raw : split(text, '\n', /*keepEmpty=*/false)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim(line.substr(0, eq));
        std::string value     = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        set(key, value);
        ++count;
    }
    return count;
}

Result<void> KeyValueConfig::loadFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Result<void>::Err(Error(1, "KeyValueConfig: cannot open file: " + path));
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    parseFromString(text);
    return Result<void>::Ok();
}

std::vector<std::pair<std::string, std::string>> KeyValueConfig::entries() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return {map_.begin(), map_.end()};
}

} // namespace zenith::core
