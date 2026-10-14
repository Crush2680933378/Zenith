// zenith::db 单测（sqlite 驱动；ZENITH_ENABLE_ORM=ON 时编译）
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "ztest.hpp"

#include "zenith/core/key_value_config.hpp"
#include "zenith/db/db_service.hpp"
#include "zenith/plugin/service_registry.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
struct User {
    int64_t id = 0;
    std::string name;
    int age = 0;
};
} // namespace

// uORM 反射注册（表名 User，主键 id 自增）
UORM_REFLECTION(User, id, name, age)

ZTEST(Db, ConfigFromKeyValue) {
    core::KeyValueConfig cfg;
    cfg.set("db.driver", "sqlite");
    cfg.set("db.database", ":memory:");
    cfg.set("db.pool_size", "2");
    const db::DbConfig c = db::DbConfig::fromConfig(cfg);
    ZEXPECT_EQ(c.driver, std::string("sqlite"));
    ZEXPECT_EQ(c.database, std::string(":memory:"));
    ZEXPECT_EQ(c.poolSize, 2);

    const auto dsc = c.toDataSourceConfig();
    ZEXPECT_EQ(dsc.params.driver, std::string("sqlite"));
    ZEXPECT_EQ(dsc.poolSize, 2);
}

ZTEST(Db, SqliteCrudAndServiceRegistry) {
    const std::string dbFile = "ztest_db.sqlite.tmp";
    std::remove(dbFile.c_str());

    core::KeyValueConfig cfg;
    cfg.set("db.driver", "sqlite");
    cfg.set("db.database", dbFile);
    cfg.set("db.pool_size", "2");

    auto svc = std::make_shared<db::DbServiceImpl>(db::DbConfig::fromConfig(cfg));

    // 注册进服务注册表（插件可查询）
    plugin::ServiceRegistry registry;
    ZEXPECT_EQ(registry.put(svc), ZENITH_OK);
    auto queried = registry.get<db::IDbService>();
    ZEXPECT_TRUE(queried != nullptr);

    uORM::Database& db = queried->database();

    // 建表 + 插入
    ZEXPECT_TRUE(db.syncTable<User>());
    User u1{0, "alice", 30};
    User u2{0, "bob", 18};
    ZEXPECT_TRUE(db.save(u1));
    ZEXPECT_TRUE(db.save(u2));
    ZEXPECT_TRUE(u1.id > 0); // 自增主键写回

    // 查询
    auto found = db.findById<User>(u1.id);
    ZEXPECT_TRUE(found.has_value());
    if (found.has_value()) {
        ZEXPECT_EQ(found->name, std::string("alice"));
        ZEXPECT_EQ(found->age, 30);
    }

    auto adults = db.find<User>("age >= ?", 20);
    ZEXPECT_EQ(adults.size(), size_t{1});

    // 条件查询构建器
    auto rows = db.query<User>().where(&User::age, uORM::Op::GT, 15).all();
    ZEXPECT_EQ(rows.size(), size_t{2});

    // 更新
    u1.age = 31;
    ZEXPECT_TRUE(db.update(u1));
    auto re = db.findById<User>(u1.id);
    ZEXPECT_EQ(re->age, 31);

    // 删除
    ZEXPECT_TRUE(db.remove(u2));
    ZEXPECT_EQ(db.find<User>("age >= ?", 0).size(), size_t{1});

    std::remove(dbFile.c_str());
}

ZTEST(Db, AsyncPostRunsOffCallerThread) {
    const std::string dbFile = "ztest_db_async.sqlite.tmp";
    std::remove(dbFile.c_str());

    core::KeyValueConfig cfg;
    cfg.set("db.driver", "sqlite");
    cfg.set("db.database", dbFile);

    db::DbServiceImpl svc(db::DbConfig::fromConfig(cfg));

    ZEXPECT_TRUE(svc.database().syncTable<User>());

    std::atomic<bool> done{false};
    std::thread::id dbThread{};
    int64_t savedId = 0;
    svc.post([&](uORM::Database& db) {
        dbThread = std::this_thread::get_id();
        User u{0, "carol", 25};
        db.save(u);
        savedId = u.id;
        done.store(true);
    });

    for (int i = 0; i < 300 && !done.load(); ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_TRUE(done.load());
    ZEXPECT_TRUE(savedId > 0);
    ZEXPECT_TRUE(dbThread != std::this_thread::get_id()); // 在工作线程执行
    auto found = svc.database().findById<User>(savedId);
    ZEXPECT_TRUE(found.has_value());

    std::remove(dbFile.c_str());
}

ZTEST_MAIN
