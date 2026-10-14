// zenith::json 单测
#include "ztest.hpp"

#include "zenith/json/json.hpp"

using namespace zenith::json;

ZTEST(Json, ScalarsRoundtrip) {
    auto r = JsonValue::parse("true");
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_TRUE(r.value().isBool());
    ZEXPECT_TRUE(r.value().asBool());

    auto i = JsonValue::parse("-42");
    ZEXPECT_TRUE(i.value().isInt());
    ZEXPECT_EQ(i.value().asInt(), int64_t{-42});

    auto d = JsonValue::parse("3.25");
    ZEXPECT_TRUE(d.value().isDouble());
    ZEXPECT_EQ(d.value().asDouble(), 3.25);

    auto big = JsonValue::parse("12345678901234567890"); // 溢出 int64 → double
    ZEXPECT_TRUE(big.value().isDouble());

    auto n = JsonValue::parse("null");
    ZEXPECT_TRUE(n.value().isNull());
}

ZTEST(Json, StringEscapesAndUnicode) {
    auto r = JsonValue::parse("\"a\\\"b\\\\c\\/\\n\\t\\u4e2d\\u6587\"");
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().asString(), std::string("a\"b\\c/\n\t中文"));

    // 代理对 → U+1F600
    auto s = JsonValue::parse("\"\\ud83d\\ude00\"");
    ZEXPECT_TRUE(static_cast<bool>(s));
    ZEXPECT_EQ(s.value().asString(), std::string("\xF0\x9F\x98\x80"));
}

ZTEST(Json, NestedContainers) {
    const std::string src = R"({"name":"zenith","stars":7,"tags":["c++","web"],
        "meta":{"license":"MIT","nested":{"deep":[1,2,3]}}})";
    auto r = JsonValue::parse(src);
    ZEXPECT_TRUE(static_cast<bool>(r));
    const JsonValue& v = r.value();
    ZEXPECT_TRUE(v.isObject());
    ZEXPECT_EQ(v["name"].asString(), std::string("zenith"));
    ZEXPECT_EQ(v["stars"].asInt(), int64_t{7});
    ZEXPECT_EQ(v["tags"].size(), size_t{2});
    ZEXPECT_EQ(v["tags"][1].asString(), std::string("web"));
    ZEXPECT_EQ(v["meta"]["license"].asString(), std::string("MIT"));
    ZEXPECT_EQ(v["meta"]["nested"]["deep"].size(), size_t{3});
    ZEXPECT_EQ(v["meta"]["nested"]["deep"][2].asInt(), int64_t{3});
}

ZTEST(Json, BuildAndDump) {
    JsonValue v = JsonValue::object();
    v["name"] = "zenith";
    v["count"] = int64_t{3};
    v["ratio"] = 0.5;
    JsonValue arr = JsonValue::array();
    arr.push_back(1);
    arr.push_back("two");
    v["items"] = std::move(arr);

    const std::string compact = v.dump();
    ZEXPECT_EQ(compact, std::string(
        R"({"count":3,"items":[1,"two"],"name":"zenith","ratio":0.5})"));

    // 回环
    auto back = JsonValue::parse(compact);
    ZEXPECT_TRUE(static_cast<bool>(back));
    ZEXPECT_EQ(back.value()["items"].size(), size_t{2});
}

ZTEST(Json, PrettyDump) {
    JsonValue v = JsonValue::object();
    v["a"] = 1;
    const std::string pretty = v.dump(2);
    ZEXPECT_TRUE(pretty.find("{\n  \"a\": 1\n}") != std::string::npos);
}

ZTEST(Json, FindAndAccess) {
    JsonValue v = JsonValue::object();
    v["x"] = 10;
    ZEXPECT_TRUE(v.has("x"));
    ZEXPECT_FALSE(v.has("y"));
    ZEXPECT_TRUE(v.find("y") == nullptr);
    ZEXPECT_EQ(v["x"].asInt(), int64_t{10});
    ZEXPECT_EQ(v["missing"].asInt(), int64_t{0}); // 读默认
    ZEXPECT_THROW(v.at(size_t{99}), std::out_of_range); // 非数组
}

ZTEST(Json, ParseErrors) {
    ZEXPECT_TRUE(JsonValue::parse("{").isErr());
    ZEXPECT_TRUE(JsonValue::parse("[1,]").isErr());
    ZEXPECT_TRUE(JsonValue::parse("01x").isErr());
    ZEXPECT_TRUE(JsonValue::parse("\"unterminated").isErr());
    ZEXPECT_TRUE(JsonValue::parse("{\"a\":1} extra").isErr());
    ZEXPECT_TRUE(JsonValue::parse("nul").isErr());
    ZEXPECT_TRUE(JsonValue::parse("\"\\u12\"").isErr());
    // 深度限制
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += "[";
    for (int i = 0; i < 200; ++i) deep += "]";
    ZEXPECT_TRUE(JsonValue::parse(deep).isErr());
    // 错误信息带位置
    auto e = JsonValue::parse("{\"a\":}");
    ZEXPECT_TRUE(e.error().message.find("offset") != std::string::npos);
}

ZTEST(Json, NumberEdgeCases) {
    ZEXPECT_EQ(JsonValue::parse("1e3").value().asDouble(), 1000.0);
    ZEXPECT_EQ(JsonValue::parse("-0.5").value().asDouble(), -0.5);
    ZEXPECT_EQ(JsonValue::parse("0").value().asInt(), int64_t{0});
    ZEXPECT_TRUE(JsonValue::parse("1.").isErr());
    ZEXPECT_TRUE(JsonValue::parse(".5").isErr());
    ZEXPECT_TRUE(JsonValue::parse("1e").isErr());
    // dump 的 double 保持 JSON 数值形态
    JsonValue d = JsonValue::parse("2.0").value();
    ZEXPECT_EQ(d.dump(), std::string("2.0"));
}

ZTEST_MAIN
