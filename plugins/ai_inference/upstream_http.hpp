// ============================================================================
//  upstream_http —— 插件内自带的最小同步 HTTP/1.1 客户端（M8.1 openai 后端）
//
//  为什么不用 zenith::http::HttpClient：插件要保持「自己解决连接」的形态
//  （与 ai_comfyui 的同步客户端同构），且需要 **流式回调**（SSE 边收边推）。
//  能力：POST/GET、Content-Length / chunked / 读到断开、SO_RCVTIMEO 超时、
//        正文分片回调（返回 false 即中止并关闭连接）。
//  局限：仅 http://（TLS 上游请交给前置反代），不做重定向。
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zenith::ai::detail {

struct UpstreamCall {
    std::string method = "POST";
    std::string url; // http://host[:port]/path
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutMs = 300000;
};

struct UpstreamResult {
    bool transportOk = false; // 连接/收包是否成功（不代表 2xx）
    int status = 0;
    std::string body; // 未走流式回调时的完整正文（chunked 已解码）
    std::string error;
};

/// 正文分片回调；返回 false 表示调用方要求中止（例如客户端已断开）
using UpstreamSink = std::function<bool(std::string_view)>;

/// 执行一次请求。onData 为空 → 完整读入 out.body；非空 → 逐片回调。
bool httpRequest(const UpstreamCall& call, const UpstreamSink& onData, UpstreamResult& out);

} // namespace zenith::ai::detail
