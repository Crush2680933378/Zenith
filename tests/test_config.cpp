// zenith::config::JsonConfig 单测
#include <fstream>

#include "ztest.hpp"

#include "zenith/config/json_config.hpp"

using namespace zenith;

namespace {
const char* kSample = R"({
    "server": {"host": "0.0.0.0", "port": 8080, "debug": true},
    "db": {"driver": "sqlite", "database": "app.db", "pool_size": 4},
    "users": [{"name": "alice"}, {"name": "bob"}],
    "ratio": 0.75,
    "count": 42,
    "note": null
})";
} // namespace

ZTEST(JsonConfig, DottedGet) {
    auto c = config::JsonConfig::fromString(kSample);
    ZEXPECT_TRUE(static_cast<bool>(c));
    ZEXPECT_EQ(c.value().get("server.host"), std::string("0.0.0.0"));
    ZEXPECT_EQ(c.value().get("server.port"), std::string("8080"));
    ZEXPECT_EQ(c.value().get("server.debug"), std::string("true"));
    ZEXPECT_EQ(c.value().get("db.driver"), std::string("sqlite"));
    ZEXPECT_EQ(c.value().get("db.pool_size"), std::string("4"));
    ZEXPECT_EQ(c.value().get("count"), std::string("42"));
    ZEXPECT_EQ(c.value().get("ratio"), std::string("0.75"));
    ZEXPECT_EQ(c.value().get("users.0.name"), std::string("alice"));
    ZEXPECT_EQ(c.value().get("users.1.name"), std::string("bob"));
    // 缺失 / null → 默认
    ZEXPECT_EQ(c.value().get("server.missing", "def"), std::string("def"));
    ZEXPECT_EQ(c.value().get("note", "fallback"), std::string("fallback"));
    ZEXPECT_EQ(c.value().get("db", "x"), std::string("x")); // 非标量
}

ZTEST(JsonConfig, Section) {
    auto c = config::JsonConfig::fromString(kSample);
    const auto sec = c.value().section("db");
    ZEXPECT_TRUE(sec.isObject());
    ZEXPECT_EQ(sec["database"].asString(), std::string("app.db"));
    ZEXPECT_TRUE(c.value().section("nope").isNull());
}

ZTEST(JsonConfig, FlattenToKeyValue) {
    auto c = config::JsonConfig::fromString(kSample);
    const core::KeyValueConfig kv = c.value().flatten();
    ZEXPECT_EQ(kv.get("server.port"), std::string("8080"));
    ZEXPECT_EQ(kv.get("db.database"), std::string("app.db"));
    ZEXPECT_EQ(kv.get("users.0.name"), std::string("alice"));
    ZEXPECT_TRUE(kv.contains("server.host"));
    // 大小写不敏感（KeyValueConfig 语义）
    ZEXPECT_EQ(kv.get("SERVER.PORT"), std::string("8080"));
}

ZTEST(JsonConfig, LoadFileAndErrors) {
    const std::string path = "ztest_config.json.tmp";
    {
        std::ofstream out(path, std::ios::binary);
        out << "{\"a\":{\"b\":1}}";
    }
    auto c = config::JsonConfig::loadFile(path);
    ZEXPECT_TRUE(static_cast<bool>(c));
    ZEXPECT_EQ(c.value().get("a.b"), std::string("1"));
    std::remove(path.c_str());

    ZEXPECT_TRUE(config::JsonConfig::loadFile("missing_config.json.tmp").isErr());
    ZEXPECT_TRUE(config::JsonConfig::fromString("{bad").isErr());
    ZEXPECT_TRUE(config::JsonConfig::fromString("[1,2]").isErr()); // 根必须是对象
}

ZTEST_MAIN
