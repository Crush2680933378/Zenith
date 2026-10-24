// ============================================================================
//  zenith::vector::vector_persist —— 集合快照落盘/加载（M13.2）
//
//  文件形态：<dir>/<collection>.zhsw（HnswIndex::serialize 平面二进制）；
//  写入走 <name>.zhsw.tmp → 原子 rename（崩溃不损坏旧快照）。
//  db_vector 插件用这对函数做定期快照与启动加载；也可独立使用。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/vector/hnsw.hpp"

namespace zenith::vector {

/// 保存单个集合（原子替换已存在快照）。dir 不存在则创建。
ZENITH_API core::Result<void> saveCollection(const std::string& dir,
                                              const std::string& collection,
                                              const HnswIndex& index);

/// 加载单个集合；快照不存在返回 Err。
ZENITH_API core::Result<std::unique_ptr<HnswIndex>>
loadCollection(const std::string& dir, const std::string& collection);

/// 列出 dir 下全部快照集合名。
ZENITH_API std::vector<std::string> listCollections(const std::string& dir);

} // namespace zenith::vector
