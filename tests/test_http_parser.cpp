// zenith::http 解析器单测（含逐字节切分）
#include "ztest.hpp"

#include "zenith/http/http_common.hpp"
#include "zenith/http/http_parser.hpp"

using namespace zenith::http;

namespace {
const char* kSimpleGet =
    "GET /index.html?name=zen%20ith&q=1&q=2 HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "User-Agent: ztest\r\n"
    "\r\n";

const char* kPostBody =
    "POST /submit HTTP/1.1\r\n"
    "Host: h\r\n"
    "Content-Length: 11\r\n"
    "\r\n"
    "hello world";
} // namespace

ZTEST(ReqParser, SimpleGet) {
    HttpRequestParser p;
    const ParseOutcome out = p.feed(kSimpleGet, std::strlen(kSimpleGet));
    ZEXPECT_TRUE(out.complete());
    const HttpRequest& r = p.request();
    ZEXPECT_TRUE(r.method == HttpMethod::Get);
    ZEXPECT_EQ(r.path, std::string("/index.html"));
    ZEXPECT_EQ(r.httpMajor * 10 + r.httpMinor, 11);
    ZEXPECT_EQ(r.header("host"), std::string("example.com")); // 大小写不敏感
    ZEXPECT_EQ(r.header("HOST"), std::string("example.com"));
    ZEXPECT_TRUE(r.keepAlive());
    // query 解码
    ZEXPECT_EQ(r.query.at("name"), std::string("zen ith"));
    ZEXPECT_EQ(r.query.at("q"), std::string("2")); // 重复键后者覆盖
}

ZTEST(ReqParser, ByteByByteSplit) {
    HttpRequestParser p;
    ParseOutcome out;
    for (const char* c = kPostBody; *c != '\0'; ++c) {
        out = p.feed(c, 1);
        if (out.failed()) break;
    }
    ZEXPECT_TRUE(out.complete());
    ZEXPECT_EQ(p.request().body, std::string("hello world"));
    ZEXPECT_EQ(p.request().contentLength(), size_t{11});
}

ZTEST(ReqParser, ChunkedBody) {
    const std::string req =
        "POST /c HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\n\r\n";
    HttpRequestParser p;
    const ParseOutcome out = p.feed(req.data(), req.size());
    ZEXPECT_TRUE(out.complete());
    ZEXPECT_EQ(p.request().body, std::string("hello world"));
}

ZTEST(ReqParser, UrlEncodedPath) {
    HttpRequestParser p;
    const char* req = "GET /files/my%20doc.txt HTTP/1.1\r\nHost: h\r\n\r\n";
    p.feed(req, std::strlen(req));
    ZEXPECT_EQ(p.request().path, std::string("/files/my doc.txt"));
}

ZTEST(ReqParser, IncompleteThenComplete) {
    HttpRequestParser p;
    const std::string head = "GET /a HTTP/1.1\r\nContent-Length: 3";
    ParseOutcome out = p.feed(head.data(), head.size());
    ZEXPECT_FALSE(out.complete());
    const std::string rest = "\r\n\r\nabc";
    out = p.feed(rest.data(), rest.size());
    ZEXPECT_TRUE(out.complete());
    ZEXPECT_EQ(p.request().body, std::string("abc"));
}

ZTEST(ReqParser, Malformed) {
    HttpRequestParser p1;
    ZEXPECT_TRUE(p1.feed("GARBAGE\r\n\r\n", 12).failed());
    HttpRequestParser p2;
    ZEXPECT_TRUE(p2.feed("GET /x FOO/1.1\r\n\r\n", 19).failed());
    HttpRequestParser p3(4); // maxBody=4
    const std::string big = "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n0123456789";
    ZEXPECT_TRUE(p3.feed(big.data(), big.size()).failed());
    HttpRequestParser p4;
    ZEXPECT_TRUE(p4.feed("GET / HTTP/1.1\r\nBadHeaderNoColon\r\n\r\n", 36).failed());
}

ZTEST(ReqParser, LeftoverForPipelining) {
    HttpRequestParser p;
    const std::string two =
        "GET /first HTTP/1.1\r\nHost: h\r\n\r\n"
        "GET /second HT";
    ParseOutcome out = p.feed(two.data(), two.size());
    ZEXPECT_TRUE(out.complete());
    const std::string leftover = p.releaseLeftover();
    ZEXPECT_EQ(leftover, std::string("GET /second HT"));
}

ZTEST(RespParser, BasicAndChunked) {
    HttpResponseParser p;
    const std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "\r\nhello";
    ParseOutcome out = p.feed(resp.data(), resp.size());
    ZEXPECT_TRUE(out.complete());
    ZEXPECT_EQ(p.response().status, 200);
    ZEXPECT_EQ(p.response().body, std::string("hello"));
    ZEXPECT_EQ(p.response().headers.get("content-type"), std::string("text/plain"));

    HttpResponseParser p2;
    const std::string chunked =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n4\r\n def\r\n0\r\n\r\n";
    out = p2.feed(chunked.data(), chunked.size());
    ZEXPECT_TRUE(out.complete());
    ZEXPECT_EQ(p2.response().body, std::string("abc def"));
}

ZTEST(HttpCommon, UrlCodecAndQuery) {
    ZEXPECT_EQ(urlDecode("a%20b+c", true), std::string("a b c"));
    ZEXPECT_EQ(urlDecode("a%20b+c", false), std::string("a b+c"));
    ZEXPECT_EQ(urlEncode("a b/c"), std::string("a%20b%2Fc"));
    ZEXPECT_EQ(urlDecode(urlEncode("你好 world")), std::string("你好 world"));

    const auto q = parseQuery("a=1&b=two%20words&c");
    ZEXPECT_EQ(q.size(), size_t{3});
    ZEXPECT_EQ(q[0].second, std::string("1"));
    ZEXPECT_EQ(q[1].second, std::string("two words"));
    ZEXPECT_EQ(q[2].second, std::string());

    ZEXPECT_TRUE(iequals("Content-Type", "content-type"));
    ZEXPECT_FALSE(iequals("abc", "abd"));
}

ZTEST(HttpCommon, StatusTextAndSerialize) {
    ZEXPECT_EQ(std::string(statusText(404)), std::string("Not Found"));
    ZEXPECT_EQ(std::string(statusText(999)), std::string("Unknown"));

    HttpResponse resp;
    resp.status = 201;
    resp.headers.set("X-Custom", "v");
    resp.body = "abc";
    const std::string wire = resp.serialize(true, true);
    ZEXPECT_TRUE(wire.find("HTTP/1.1 201 Created\r\n") == 0);
    ZEXPECT_TRUE(wire.find("X-Custom: v\r\n") != std::string::npos);
    ZEXPECT_TRUE(wire.find("Content-Length: 3\r\n") != std::string::npos);
    ZEXPECT_TRUE(wire.find("Connection: keep-alive\r\n") != std::string::npos);
    ZEXPECT_TRUE(wire.size() >= 7);
    if (wire.size() >= 7) {
        ZEXPECT_EQ(wire.substr(wire.size() - 7), std::string("\r\n\r\nabc"));
    }

    const std::string headOnly = resp.serialize(false, false);
    ZEXPECT_TRUE(headOnly.find("Connection: close") != std::string::npos);
    ZEXPECT_TRUE(headOnly.find("\r\n\r\nabc") == std::string::npos);
}

ZTEST_MAIN
