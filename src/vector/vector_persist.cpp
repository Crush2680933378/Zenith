#include "zenith/vector/vector_persist.hpp"

#include <cstdio>
#include <filesystem>

namespace zenith::vector {

namespace fs = std::filesystem;

core::Result<void> saveCollection(const std::string& dir, const std::string& collection,
                                  const HnswIndex& index) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string finalPath = dir + "/" + collection + ".zhsw";
    const std::string tmpPath = finalPath + ".tmp";

    auto blob = index.serialize();
    if (!blob) {
        return core::Result<void>::Err(std::move(blob).error());
    }
    FILE* f = std::fopen(tmpPath.c_str(), "wb");
    if (f == nullptr) {
        return core::Result<void>::Err(
            core::Error(310, "persist: cannot open " + tmpPath));
    }
    const size_t n = std::fwrite(blob.value().data(), 1, blob.value().size(), f);
    std::fclose(f);
    if (n != blob.value().size()) {
        fs::remove(tmpPath, ec);
        return core::Result<void>::Err(core::Error(311, "persist: short write"));
    }
    fs::rename(tmpPath, finalPath, ec); // 原子替换
    if (ec) {
        fs::remove(finalPath, ec);
        return core::Result<void>::Err(
            core::Error(312, "persist: rename failed: " + ec.message()));
    }
    return core::Result<void>::Ok();
}

core::Result<std::unique_ptr<HnswIndex>> loadCollection(const std::string& dir,
                                                        const std::string& collection) {
    const std::string path = dir + "/" + collection + ".zhsw";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return core::Result<std::unique_ptr<HnswIndex>>::Err(
            core::Error(313, "persist: no snapshot for '" + collection + "'"));
    }
    std::string blob;
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        blob.append(buf, n);
    }
    std::fclose(f);
    return HnswIndex::deserialize(reinterpret_cast<const uint8_t*>(blob.data()),
                                  blob.size());
}

std::vector<std::string> listCollections(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string name = e.path().filename().string();
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".zhsw") == 0) {
            out.push_back(name.substr(0, name.size() - 5));
        }
    }
    return out;
}

} // namespace zenith::vector
