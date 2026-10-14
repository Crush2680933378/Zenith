// ============================================================================
//  zenith::db —— uORM 服务化封装（M3 数据层）
//  - DbConfig：从 Zenith 配置（KeyValueConfig/JsonConfig 拼平）读取连接参数
//  - IDbService：注册进 ServiceRegistry 的数据库服务接口
//    * 同步入口：database()（阻塞调用方线程）
//    * 异步入口：post()——DB 阻塞操作投递到专用线程池，绝不占用 EventLoop 线程
//  - 连接池由 uORM::DataSource 提供（有界池 + 获取超时 + 空闲 ping）
//  依赖：ZENITH_ENABLE_ORM=ON（thirdparty/uORM 子模块）
// ============================================================================
#pragma once

#include <functional>
#include <memory>

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/export.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/core/result.hpp"
#include "zenith/plugin/service.hpp"

#include "uORM/orm/Database.h"
#include "uORM/driver/DataSource.h"

namespace zenith::db {

/// 数据库连接配置（键前缀默认 "db"，如 db.driver / db.host / db.database）
struct DbConfig {
    std::string driver = "sqlite";     // sqlite | mysql | postgresql
    std::string host = "127.0.0.1";
    int port = 0;                      // sqlite 忽略
    std::string username;
    std::string password;
    std::string database = ":memory:"; // sqlite：文件路径或 :memory:
    int poolSize = 4;

    /// 从配置读取（大小写不敏感键）。
    static DbConfig fromConfig(const core::KeyValueConfig& cfg,
                               const std::string& prefix = "db");

    /// 转为 uORM 数据源配置。
    [[nodiscard]] uORM::DataSourceConfig toDataSourceConfig() const;
};

class IDbService : public plugin::IService {
public:
    ZENITH_SERVICE_DECL(IDbService, "zenith.db.IDbService", 1)

    /// 同步入口：直接使用 uORM（阻塞调用线程；勿在 EventLoop 线程调用）。
    virtual uORM::Database& database() = 0;
    virtual uORM::DataSource& source() = 0;

    /// 异步入口：把阻塞 DB 操作投递到专用线程池执行。
    virtual void post(std::function<void(uORM::Database&)> fn) = 0;
};

class ZENITH_API DbServiceImpl final : public IDbService {
public:
    /// workerThreads：专用 DB 线程数（默认 2）。
    DbServiceImpl(DbConfig cfg, size_t workerThreads = 2);
    ~DbServiceImpl() override = default;

    uORM::Database& database() override { return db_; }
    uORM::DataSource& source() override { return ds_; }
    void post(std::function<void(uORM::Database&)> fn) override;

private:
    uORM::DataSource ds_;
    uORM::Database db_;
    std::unique_ptr<concurrency::ThreadPool> workers_;
};

} // namespace zenith::db
