// core 模块单测：Result / Buffer / string / time / KeyValueConfig
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include "ztest.hpp"

#include "zenith/core/buffer.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/core/result.hpp"
#include "zenith/core/string.hpp"
#include "zenith/core/time_util.hpp"

using namespace zenith::core;

/* ---------------- Result ---------------- */

ZTEST(Result, OkAndValue) {
    auto r = Result<int>::Ok(42);
    ZEXPECT_TRUE(r.isOk());
    ZEXPECT_FALSE(r.isErr());
    ZEXPECT_EQ(r.value(), 42);
    ZEXPECT_EQ(r.valueOr(0), 42);
}

ZTEST(Result, ErrAndError) {
    auto r = Result<int>::Err(Error(7, "boom"));
    ZEXPECT_TRUE(r.isErr());
    ZEXPECT_EQ(r.error().code, 7);
    ZEXPECT_EQ(r.error().message, std::string("boom"));
    ZEXPECT_EQ(r.valueOr(-1), -1);
}

ZTEST(Result, VoidSpecialization) {
    auto ok = Result<void>::Ok();
    auto err = Result<void>::Err(Error(1, "e"));
    ZEXPECT_TRUE(static_cast<bool>(ok));
    ZEXPECT_TRUE(ok.isOk());
    ZEXPECT_TRUE(err.isErr());
    ZEXPECT_EQ(err.error().code, 1);
}

ZTEST(Result, ExplicitBoolConversion) {
    const auto f = []() -> Result<int> { return Result<int>::Err(Error(0, "x")); };
    ZEXPECT_FALSE(static_cast<bool>(f()));
}

/* ---------------- Buffer ---------------- */

ZTEST(Buffer, AppendAndReadString) {
    Buffer b;
    b.appendString("hello ");
    b.appendString("world");
    ZEXPECT_EQ(b.readableBytes(), size_t{11});
    ZEXPECT_EQ(b.readAllAsString(), std::string("hello world"));
    ZEXPECT_EQ(b.readableBytes(), size_t{0});
}

ZTEST(Buffer, EndianRead) {
    Buffer b;
    const uint8_t raw[4] = {0x12, 0x34, 0x56, 0x78};
    b.append(raw, sizeof(raw));
    ZEXPECT_EQ(b.readUint32BE(), uint32_t{0x12345678u});

    Buffer b2;
    b2.append(raw, sizeof(raw));
    ZEXPECT_EQ(b2.readUint32LE(), uint32_t{0x78563412u});
}

ZTEST(Buffer, ConsumeAndPeek) {
    Buffer b;
    b.appendString("abcdef");
    b.consume(3);
    ZEXPECT_EQ(std::string(reinterpret_cast<const char*>(b.peek()), b.readableBytes()),
               std::string("def"));
    ZEXPECT_EQ(b.readAllAsString(), std::string("def"));
}

ZTEST(Buffer, GrowsAcrossCapacity) {
    Buffer b(8);
    std::string expected;
    for (int i = 0; i < 100; ++i) {
        const std::string chunk = "0123456789";
        b.appendString(chunk);
        expected += chunk;
    }
    ZEXPECT_EQ(b.readAllAsString().size(), expected.size());
}

ZTEST(Buffer, ReadStringUnderflowThrows) {
    Buffer b;
    b.appendString("ab");
    ZEXPECT_THROW(b.readString(5), std::out_of_range);
}

/* ---------------- string_util ---------------- */

ZTEST(StringUtil, Split) {
    const auto parts = string_util::split("a,b,,c", ',');
    ZEXPECT_EQ(parts.size(), size_t{3});
    ZEXPECT_EQ(parts[0], std::string("a"));
    ZEXPECT_EQ(parts[2], std::string("c"));

    const auto keep = string_util::split("a,b,,c", ',', true);
    ZEXPECT_EQ(keep.size(), size_t{4});
}

ZTEST(StringUtil, Trim) {
    ZEXPECT_EQ(string_util::trim("  hi \r\n"), std::string("hi"));
    ZEXPECT_EQ(string_util::trimLeft("  x"), std::string("x"));
    ZEXPECT_EQ(string_util::trimRight("x  "), std::string("x"));
    ZEXPECT_EQ(string_util::trim("   "), std::string());
}

ZTEST(StringUtil, CaseAndAffix) {
    ZEXPECT_EQ(string_util::toLower("HeLLo-42"), std::string("hello-42"));
    ZEXPECT_EQ(string_util::toUpper("HeLLo"), std::string("HELLO"));
    ZEXPECT_TRUE(string_util::startsWith("zenith.md", "zen"));
    ZEXPECT_TRUE(string_util::endsWith("zenith.md", ".md"));
    ZEXPECT_FALSE(string_util::startsWith("z", "zen"));
}

ZTEST(StringUtil, ReplaceAll) {
    ZEXPECT_EQ(string_util::replaceAll("a-b-c", "-", "+"), std::string("a+b+c"));
    ZEXPECT_EQ(string_util::replaceAll("aaa", "aa", "b"), std::string("ba"));
}

/* ---------------- time_util ---------------- */

ZTEST(TimeUtil, FormatTimestampShape) {
    const std::string s = formatTimestamp(1760000000123LL);
    ZEXPECT_EQ(s.size(), size_t{23}); // "YYYY-MM-DD HH:MM:SS.mmm"
    ZEXPECT_EQ(s[4], '-');
    ZEXPECT_EQ(s[10], ' ');
    ZEXPECT_EQ(s[19], '.');
}

ZTEST(TimeUtil, MonotonicClockAdvances) {
    const int64_t a = nowMilliseconds();
    const int64_t b = nowMilliseconds();
    ZEXPECT_TRUE(b >= a);
}

/* ---------------- KeyValueConfig ---------------- */

ZTEST(Config, ParseAndCaseInsensitive) {
    KeyValueConfig cfg;
    const size_t n = cfg.parseFromString(
        "# comment\n"
        "server.port = 8080\n"
        "; another comment\n"
        "  log.level  =  debug  \n"
        "novalue\n");
    ZEXPECT_EQ(n, size_t{2});
    ZEXPECT_EQ(cfg.get("SERVER.PORT"), std::string("8080"));
    ZEXPECT_EQ(cfg.get("log.level"), std::string("debug")); // 值保留原样，key 归一
    ZEXPECT_EQ(cfg.get("missing", "fallback"), std::string("fallback"));
    ZEXPECT_TRUE(cfg.contains("server.port"));
    ZEXPECT_FALSE(cfg.contains("nope"));
}

ZTEST(Config, LoadFromFile) {
    const std::string path = "ztest_kv_config.tmp";
    {
        std::ofstream out(path, std::ios::binary);
        out << "alpha=1\nbeta = two\n";
    }
    KeyValueConfig cfg;
    auto r = cfg.loadFromFile(path);
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(cfg.get("alpha"), std::string("1"));
    ZEXPECT_EQ(cfg.get("beta"), std::string("two"));
    std::remove(path.c_str());

    auto missing = KeyValueConfig{}.loadFromFile("definitely_missing_file.tmp");
    ZEXPECT_TRUE(missing.isErr());
}

ZTEST_MAIN
