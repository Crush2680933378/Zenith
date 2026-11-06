// ============================================================================
//  zenith::http::grpc —— 极简 gRPC 服务端（跑在 HTTP/2 上，不做 protobuf 编解码）
//
//  设计取舍：protobuf 的序列化是业务自己的事，这一层只负责 gRPC 的**传输契约**：
//    - POST + content-type: application/grpc
//    - 消息帧：1 字节压缩标志 + 4 字节大端长度 + 负载
//    - 响应：头里 content-type，正文是同样的消息帧，
//      状态走 **trailers**（grpc-status / grpc-message）
//
//  支持：一元调用（unary）。不支持：流式（client/server/bidi streaming）、压缩。
// ============================================================================
#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "zenith/core/export.hpp"

namespace zenith::http {

class HttpServer;
class Context;

namespace grpc {

/// gRPC 状态码（只列常用的）
enum class Code : int {
    Ok = 0,
    Cancelled = 1,
    Unknown = 2,
    InvalidArgument = 3,
    DeadlineExceeded = 4,
    NotFound = 5,
    AlreadyExists = 6,
    PermissionDenied = 7,
    ResourceExhausted = 8,
    FailedPrecondition = 9,
    Aborted = 10,
    OutOfRange = 11,
    Unimplemented = 12,
    Internal = 13,
    Unavailable = 14,
    DataLoss = 15,
    Unauthenticated = 16,
};

/// 一次调用的入参
struct Call {
    std::string method;   // "/pkg.Service/Method"
    std::string message;  // 已去掉 5 字节帧头的原始负载
    /// 元数据（HTTP/2 头里除伪头外的部分）
    std::map<std::string, std::string> metadata;
};

using Handler = std::function<Code(const Call&, std::string& response)>;

/* --------------------------- 消息帧编解码 --------------------------- */

ZENITH_API std::string encodeMessage(std::string_view payload);

struct MessageResult {
    enum class Status { Pending, Ok, Error };
    Status status = Status::Pending;
    std::string payload;
    size_t consumed = 0;
    std::string error;
};

ZENITH_API MessageResult decodeMessage(std::string_view buf, size_t maxSize = 4 * 1024 * 1024);

/* ----------------------------- 服务注册 ----------------------------- */

class ZENITH_API Service {
public:
    /// fullPath 形如 "/helloworld.Greeter/SayHello"
    Service& method(std::string fullPath, Handler handler);

    /// 注册到服务器：为每个方法挂一条 POST 路由
    void mount(HttpServer& server);

    /// 处理方法（供 Router 直接回调；也便于单测）
    void handle(const std::string& method, Context& ctx);

private:
    std::map<std::string, Handler> handlers_;
};

/// 把 HTTP/1 的 "application/grpc" 判断抽出来（h2 与 h1 都能用）
ZENITH_API bool isGrpcContentType(std::string_view contentType);

}  // namespace grpc
}  // namespace zenith::http
