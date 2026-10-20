// zenith::http::Router 单测（纯逻辑，不起网络）
#include "ztest.hpp"

#include <sstream>

#include "zenith/http/router.hpp"

using namespace zenith::http;

namespace {
Handler echoParam(const std::string& key) {
    return [key](Context& c) { c.text(200, c.param(key)); };
}

Context makeCtx(HttpMethod m, std::string path) {
    HttpRequest rq;
    rq.method = m;
    rq.path = std::move(path);
    return Context(std::move(rq));
}
} // namespace

ZTEST(Router, StaticMatch) {
    Router r;
    r.get("/healthz", [](Context& c) { c.text(200, "ok"); });

    Context ctx = makeCtx(HttpMethod::Get, "/healthz");
    r.dispatch(ctx);
    ZEXPECT_EQ(ctx.responseStatus(), 200);
    ZEXPECT_EQ(ctx.response().body, std::string("ok"));
}

ZTEST(Router, ParamAndCatchAll) {
    Router r;
    r.get("/users/:id", echoParam("id"));
    r.get("/users/:id/posts/:pid", [](Context& c) {
        c.text(200, c.param("id") + "-" + c.param("pid"));
    });
    r.get("/files/*path", echoParam("path"));

    auto c1 = makeCtx(HttpMethod::Get, "/users/42");
    r.dispatch(c1);
    ZEXPECT_EQ(c1.responseStatus(), 200);
    ZEXPECT_EQ(c1.response().body, std::string("42"));

    auto c2 = makeCtx(HttpMethod::Get, "/users/7/posts/9");
    r.dispatch(c2);
    ZEXPECT_EQ(c2.response().body, std::string("7-9"));

    auto c3 = makeCtx(HttpMethod::Get, "/files/a/b/c.txt");
    r.dispatch(c3);
    ZEXPECT_EQ(c3.response().body, std::string("a/b/c.txt"));

    auto c4 = makeCtx(HttpMethod::Get, "/nope");
    r.dispatch(c4);
    ZEXPECT_EQ(c4.responseStatus(), 404);
}

ZTEST(Router, StaticWinsOverParam) {
    Router r;
    r.get("/users/admin", [](Context& c) { c.text(200, "static"); });
    r.get("/users/:name", [](Context& c) { c.text(200, "param"); });

    auto run = [&r](const char* path) {
        Context ctx = makeCtx(HttpMethod::Get, path);
        r.dispatch(ctx);
        return ctx.response().body;
    };
    ZEXPECT_EQ(run("/users/admin"), std::string("static"));
    ZEXPECT_EQ(run("/users/alice"), std::string("param"));
}

ZTEST(Router, MethodNotAllowedAndHeadFallback) {
    Router r;
    r.post("/only-post", [](Context& c) { c.text(201, "created"); });

    Context get405 = makeCtx(HttpMethod::Get, "/only-post");
    r.dispatch(get405);
    ZEXPECT_EQ(get405.responseStatus(), 405);

    // GET 路由自动兼容 HEAD（M2 语义）
    Router r2;
    r2.get("/data", [](Context& c) { c.text(200, "body"); });
    Context head = makeCtx(HttpMethod::Head, "/data");
    r2.dispatch(head);
    ZEXPECT_EQ(head.responseStatus(), 200);
}

ZTEST(Router, MiddlewareChainOrder) {
    Router r;
    std::vector<std::string> order;

    r.use([&](Context& c) {
        order.push_back("global-pre");
        c.next();
        order.push_back("global-post");
    });
    r.get("/x", [](Context& c) { c.text(200, "x"); },
          {[&](Context& c) {
               order.push_back("route-pre");
               c.next();
               order.push_back("route-post");
           }});

    Context ctx = makeCtx(HttpMethod::Get, "/x");
    r.dispatch(ctx);
    ZEXPECT_EQ(order.size(), size_t{4});
    ZEXPECT_EQ(order[0], std::string("global-pre"));
    ZEXPECT_EQ(order[1], std::string("route-pre"));
    ZEXPECT_EQ(order[2], std::string("route-post"));
    ZEXPECT_EQ(order[3], std::string("global-post"));
}

ZTEST(Router, MiddlewareShortCircuit) {
    Router r;
    r.use([](Context& c) { c.text(401, "denied"); /* 不调 next */ });
    r.get("/secret", [](Context& c) { c.text(200, "should not run"); });

    Context ctx = makeCtx(HttpMethod::Get, "/secret");
    r.dispatch(ctx);
    ZEXPECT_EQ(ctx.responseStatus(), 401);
    ZEXPECT_EQ(ctx.response().body, std::string("denied"));
}

ZTEST(Router, HandlerNotWritingBodyYields200Empty) {
    Router r;
    r.get("/noop", [](Context&) {});

    Context ctx = makeCtx(HttpMethod::Get, "/noop");
    r.dispatch(ctx);
    ZEXPECT_EQ(ctx.responseStatus(), 200);
    ZEXPECT_TRUE(ctx.response().body.empty());
}

ZTEST(Router, Group) {
    Router r;
    auto api = r.group("/api/v1");
    api.use([](Context& c) { c.setHeader("X-Group", "v1"); c.next(); });
    api.get("/users/:id", echoParam("id"));

    Context ok = makeCtx(HttpMethod::Get, "/api/v1/users/5");
    r.dispatch(ok);
    ZEXPECT_EQ(ok.responseStatus(), 200);
    ZEXPECT_EQ(ok.response().body, std::string("5"));
    ZEXPECT_EQ(ok.response().headers.get("X-Group"), std::string("v1"));
}

ZTEST(Router, HasRoute) {
    Router r;
    r.get("/a/b", [](Context&) {});
    ZEXPECT_TRUE(r.hasRoute("/a/b"));
    ZEXPECT_FALSE(r.hasRoute("/a/c"));
}

/// 根路径与 catch-all 的边界（Blog 实战固化的规格）：
/// 1) catch-all "/*path" 不匹配 "/"——path 语义是「/ 之后至少一段」，
///    空剩余段不算命中。SPA 托管忘注册 "/" 的直接症状就是首页 404。
/// 2) 显式注册 "/" 后正常命中；且静态段优先级仍高于 catch-all。
ZTEST(Router, RootPathNotMatchedByCatchAll) {
    Router r;
    r.get("/api/stats", [](Context& c) { c.text(200, "stats"); });
    r.get("/*path", [](Context& c) { c.text(200, "spa:" + c.param("path")); });

    // "/"：catch-all 未命中 → 404（这就是必须单独注册根路径的原因）
    Context root = makeCtx(HttpMethod::Get, "/");
    r.dispatch(root);
    ZEXPECT_EQ(root.responseStatus(), 404);

    // 显式注册 "/" 后命中
    r.get("/", [](Context& c) { c.text(200, "index"); });
    Context root2 = makeCtx(HttpMethod::Get, "/");
    r.dispatch(root2);
    ZEXPECT_EQ(root2.responseStatus(), 200);
    ZEXPECT_EQ(root2.response().body, std::string("index"));

    // 静态段仍优先于 catch-all
    Context api = makeCtx(HttpMethod::Get, "/api/stats");
    r.dispatch(api);
    ZEXPECT_EQ(api.response().body, std::string("stats"));

    // 多级路径仍由 catch-all 兜底
    Context deep = makeCtx(HttpMethod::Get, "/game/room/ABCD");
    r.dispatch(deep);
    ZEXPECT_EQ(deep.response().body, std::string("spa:game/room/ABCD"));
}

ZTEST_MAIN
