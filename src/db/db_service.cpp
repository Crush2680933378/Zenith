#include "zenith/db/db_service.hpp"

namespace zenith::db {

DbConfig DbConfig::fromConfig(const core::KeyValueConfig& cfg, const std::string& prefix) {
    DbConfig c;
    const std::string p = prefix.empty() ? std::string("db") : prefix;
    c.driver = cfg.get(p + ".driver", c.driver);
    c.host = cfg.get(p + ".host", c.host);
    const std::string portStr = cfg.get(p + ".port", "0");
    c.port = std::atoi(portStr.c_str());
    c.username = cfg.get(p + ".username", c.username);
    c.password = cfg.get(p + ".password", c.password);
    c.database = cfg.get(p + ".database", c.database);
    const std::string poolStr = cfg.get(p + ".pool_size", "4");
    c.poolSize = std::atoi(poolStr.c_str());
    return c;
}

uORM::DataSourceConfig DbConfig::toDataSourceConfig() const {
    uORM::DataSourceConfig dsc;
    dsc.params.driver = driver;
    dsc.params.host = host;
    dsc.params.port = port;
    dsc.params.username = username;
    dsc.params.password = password;
    dsc.params.database = database;
    dsc.poolSize = poolSize > 0 ? poolSize : 4;
    dsc.name = "zenith-default";
    return dsc;
}

DbServiceImpl::DbServiceImpl(DbConfig cfg, size_t workerThreads)
    : ds_(cfg.toDataSourceConfig()), db_(ds_) {
    workers_ = std::make_unique<concurrency::ThreadPool>(workerThreads);
}

void DbServiceImpl::post(std::function<void(uORM::Database&)> fn) {
    // 拷贝 db 引用包裹（this 生命周期由服务注册表保证）
    uORM::Database* db = &db_;
    workers_->post([db, f = std::move(fn)] { f(*db); });
}

} // namespace zenith::db
