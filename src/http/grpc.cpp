// ============================================================================
//  极简 gRPC 服务端实现
// ============================================================================
#include "zenith/http/grpc.hpp"

#include "zenith/http/context.hpp"
#include "zenith/http/http_server.hpp"

namespace zenith::http::grpc {

std::string encodeMessage(std::string_view payload) {
    std::string out;
    out.reserve(5 + payload.size());
    out.push_back('\0');  // compressed-flag = 0（不压缩）
    const uint32_t n = static_cast<uint32_t>(payload.size());
    out.push_back(static_cast<char>((n >> 24) & 0xFF));
    out.push_back(static_cast<char>((n >> 16) & 0xFF));
    out.push_back(static_cast<char>((n >> 8) & 0xFF));
    out.push_back(static_cast<char>(n & 0xFF));
    out.append(payload.data(), payload.size());
    return out;
}

MessageResult decodeMessage(std::string_view buf, size_t maxSize) {
    MessageResult r;
    if (buf.size() < 5) return r;  // Pending
    if (static_cast<uint8_t>(buf[0]) != 0) {
        r.status = MessageResult::Status::Error;
        r.error = "compressed grpc message not supported";
        return r;
    }
    uint32_t len = 0;
    for (int i = 1; i <= 4; ++i) {
        len = (len << 8) | static_cast<uint32_t>(static_cast<uint8_t>(buf[i]));
    }
    if (len > maxSize) {
        r.status = MessageResult::Status::Error;
        r.error = "grpc message too large";
        return r;
    }
    if (buf.size() < 5 + len) return r;  // Pending
    r.payload.assign(buf.data() + 5, len);
    r.consumed = 5 + static_cast<size_t>(len);
    r.status = MessageResult::Status::Ok;
    return r;
}

bool isGrpcContentType(std::string_view contentType) {
    return contentType.rfind("application/grpc", 0) == 0;
}

Service& Service::method(std::string fullPath, Handler handler) {
    handlers_[std::move(fullPath)] = std::move(handler);
    return *this;
}

void Service::mount(HttpServer& server) {
    // 不逐条注册路由：未注册的方法也必须能回 grpc-status=12（UNIMPLEMENTED），
    // 而普通 404 会把尾随头丢掉。所以挂在路由之前的钩子上。
    server.setGrpcHandler([this](Context& c) {
        if (!isGrpcContentType(c.request().headers.get("content-type"))) return false;
        handle(c.request().path, c);
        return true;
    });
}

void Service::handle(const std::string& method, Context& ctx) {
    Code code = Code::Ok;
    std::string errMsg;
    std::string respBody;

    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
        code = Code::Unimplemented;
        errMsg = "method not found: " + method;
    } else {
        Call call;
        call.method = method;
        auto decoded = decodeMessage(ctx.request().body);
        if (decoded.status != MessageResult::Status::Ok) {
            code = Code::Internal;
            errMsg = decoded.error.empty() ? "malformed grpc frame" : decoded.error;
        } else {
            call.message = std::move(decoded.payload);
            for (const auto& [k, v] : ctx.request().headers.entries())
                call.metadata[k] = v;
            code = it->second(call, respBody);
        }
    }

    ctx.status(200).setHeader("content-type", "application/grpc");
    if (code == Code::Ok) {
        ctx.response().body = encodeMessage(respBody);
    } else {
        ctx.response().body.clear();
        ctx.trailer("grpc-message", errMsg.empty() ? std::string("error") : errMsg);
    }
    // grpc-status 必须走 trailers（HTTP/2 的尾随 HEADERS），不能当普通响应头
    ctx.trailer("grpc-status", std::to_string(static_cast<int>(code)));
}

}  // namespace zenith::http::grpc
