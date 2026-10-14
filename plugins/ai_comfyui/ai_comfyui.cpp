// ============================================================================
//  ai_comfyui —— C ABI 插件：ComfyUI AI 绘画服务
//  自包含同步 HTTP/1.1 客户端（原生 socket，工作线程执行）——纯 C 边界自治。
//  流程：占位符注入 → POST /prompt → 轮询 /history/{id} → GET /view 落盘。
//  测试：tests/test_ai.cpp 用 Zenith 自建 mock ComfyUI（HttpServer）全链路验证。
// ============================================================================
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <thread>

#include "zenith/abi/zenith_abi.h"

#include "ai_service.hpp"

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using SocketFd = SOCKET;
  #define AI_INVALID INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketFd = int;
  #define AI_INVALID (-1)
#endif

namespace {

const zenith_host_api* g_host = nullptr;

void logInfo(const std::string& m) {
    if (g_host) g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, "ai_comfyui", 0, m.c_str());
}
void logError(const std::string& m) {
    if (g_host) g_host->log_write(g_host->log_ctx, ZENITH_LOG_ERROR, "ai_comfyui", 0, m.c_str());
}

#if defined(_WIN32)
struct WsaInit {
    WsaInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
#endif

// ---------------------------------------------------------------------------
// 极简同步 HTTP 客户端（GET/POST，Content-Length 响应；无 keep-alive）
// ---------------------------------------------------------------------------
struct HttpReply {
    int status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

bool httpExec(const std::string& host, uint16_t port, const std::string& method,
              const std::string& path, const std::string& body, HttpReply& out) {
#if defined(_WIN32)
    static WsaInit wsaInit;
#endif
    const SocketFd fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == AI_INVALID) return false;

    // 解析 host（数字优先，DNS 兜底）
    in_addr addr{};
    if (inet_pton(AF_INET, host.c_str(), &addr) != 1) {
        addrinfo* res = nullptr;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
            return false;
        }
        addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
#if defined(_WIN32)
        closesocket(fd);
#else
        ::close(fd);
#endif
        return false;
    }

    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                      std::to_string(port) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < req.size()) {
        const int n = ::send(fd, req.data() + sent, static_cast<int>(req.size() - sent), 0);
        if (n <= 0) {
#if defined(_WIN32)
            closesocket(fd);
#else
            ::close(fd);
#endif
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    std::string all;
    char buf[16384];
    for (;;) {
        const int n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        all.append(buf, static_cast<size_t>(n));
    }
#if defined(_WIN32)
    closesocket(fd);
#else
    ::close(fd);
#endif

    // 解析状态行 + 头 + 体
    const size_t headEnd = all.find("\r\n\r\n");
    if (headEnd == std::string::npos) return false;
    const std::string head = all.substr(0, headEnd);
    out.body = all.substr(headEnd + 4);
    const size_t sp1 = head.find(' ');
    if (sp1 == std::string::npos) return false;
    out.status = std::atoi(head.c_str() + sp1 + 1);

    // 去除 chunked（ComfyUI 可能启用）：极简拼接
    const size_t te = head.find("Transfer-Encoding: chunked");
    const size_t te2 = head.find("transfer-encoding: chunked");
    if (te != std::string::npos || te2 != std::string::npos) {
        std::string plain;
        size_t pos = 0;
        while (pos < out.body.size()) {
            const size_t nl = out.body.find("\r\n", pos);
            if (nl == std::string::npos) break;
            const long len = strtol(out.body.substr(pos, nl - pos).c_str(), nullptr, 16);
            if (len <= 0) break;
            plain.append(out.body, nl + 2, static_cast<size_t>(len));
            pos = nl + 2 + static_cast<size_t>(len) + 2;
        }
        out.body = std::move(plain);
    }
    return true;
}

// 极简 JSON 字符串字段提取
std::string jsonStr(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (p < j.size() && j[p] == '"') {
        const size_t e = j.find('"', p + 1);
        return e == std::string::npos ? "" : j.substr(p + 1, e - p - 1);
    }
    const size_t e = j.find_first_of(",}", p);
    return j.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

/// 占位符注入：{{seed}} / {{key}}
std::string applyPlaceholders(const std::string& wf,
                              const zenith::ai::PlaceholderResolver& r) {
    std::string out;
    out.reserve(wf.size());
    for (size_t i = 0; i < wf.size();) {
        if (wf.compare(i, 2, "{{") == 0) {
            const size_t e = wf.find("}}", i + 2);
            if (e != std::string::npos) {
                const std::string key = wf.substr(i + 2, e - i - 2);
                if (key == "seed") {
                    static std::mt19937 rng{std::random_device{}()};
                    out += std::to_string(rng() % 1000000000u);
                } else if (r) {
                    out += r(key);
                } else {
                    out += "";
                }
                i = e + 2;
                continue;
            }
        }
        out.push_back(wf[i]);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
class AiDrawImpl final : public zenith::ai::IAiDrawService {
public:
    explicit AiDrawImpl(std::string host, uint16_t port)
        : host_(std::move(host)), port_(port) {}

    zenith::ai::DrawResult draw(const std::string& workflowJson,
                                const std::string& outputDir,
                                zenith::ai::PlaceholderResolver resolver,
                                int timeoutSec) override {
        zenith::ai::DrawResult r;
        const std::string wf = applyPlaceholders(workflowJson, resolver);

        // 1) POST /prompt
        HttpReply resp;
        if (!httpExec(host_, port_, "POST", "/prompt", wf, resp)) {
            r.error = "connect/request failed to " + host_ + ":" + std::to_string(port_);
            return r;
        }
        if (!resp.ok()) {
            r.error = "/prompt rejected: HTTP " + std::to_string(resp.status);
            return r;
        }
        r.promptId = jsonStr(resp.body, "prompt_id");
        if (r.promptId.empty()) {
            r.error = "no prompt_id in /prompt response";
            return r;
        }

        // 2) 轮询 /history/{id}
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(timeoutSec);
        std::string outputsJson;
        for (;;) {
            if (std::chrono::steady_clock::now() > deadline) {
                r.error = "timeout waiting for prompt " + r.promptId;
                return r;
            }
            HttpReply hist;
            if (!httpExec(host_, port_, "GET", "/history/" + r.promptId, "", hist) || !hist.ok()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            // {"<prompt_id>": {"outputs": {...}, "status": {"completed": true, ...}}}
            if (hist.body.find("\"status\"") != std::string::npos &&
                hist.body.find("\"completed\":true") != std::string::npos) {
                outputsJson = hist.body;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // 3) 下载产物：/view?filename=..&subfolder=..&type=output
        //    从 outputs 里找 filename（极简扫描）
        size_t pos = 0;
        while ((pos = outputsJson.find("\"filename\":", pos)) != std::string::npos) {
            const std::string file = jsonStr(outputsJson.substr(pos), "filename");
            pos += 11;
            if (file.empty() || file.size() < 4 ||
                (file.compare(file.size() - 4, 4, ".png") != 0 &&
                 file.compare(file.size() - 4, 4, ".jpg") != 0 &&
                 file.compare(file.size() - 5, 5, ".jpeg") != 0 &&
                 file.compare(file.size() - 4, 4, ".webp") != 0)) {
                continue;
            }
            HttpReply img;
            if (httpExec(host_, port_, "GET",
                         "/view?filename=" + file + "&type=output", "", img) &&
                img.ok() && !img.body.empty()) {
                const std::string path = outputDir + "/" + file;
                FILE* f = std::fopen(path.c_str(), "wb");
                if (f != nullptr) {
                    std::fwrite(img.body.data(), 1, img.body.size(), f);
                    std::fclose(f);
                    r.outputs.push_back(path);
                }
            }
        }
        r.ok = !r.outputs.empty();
        if (!r.ok) r.error = "no downloadable outputs";
        logInfo("comfyui draw " + r.promptId + ": " + std::to_string(r.outputs.size()) + " file(s)");
        return r;
    }

    std::string submit(const std::string& workflowJson,
                       const std::string& outputDir,
                       zenith::ai::PlaceholderResolver resolver,
                       std::function<void(zenith::ai::DrawResult)> onDone,
                       ProgressCallback onProgress) override {
        auto wfCopy = std::make_shared<std::string>(workflowJson);
        auto dirCopy = std::make_shared<std::string>(outputDir);
        auto resCopy = std::make_shared<zenith::ai::PlaceholderResolver>(resolver);
        auto doneCopy = std::make_shared<std::function<void(zenith::ai::DrawResult)>>(onDone);
        workers_.emplace_back([this, wfCopy, dirCopy, resCopy, doneCopy, onProgress] {
            (void)onProgress; // M4：进度回调预留（WS /ws 接入在 M4.4 后续）
            auto r = draw(*wfCopy, *dirCopy, *resCopy, 300);
            if (*doneCopy) (*doneCopy)(r);
        });
        workers_.back().detach();
        return ""; // 异步句柄 M4 简化：经 onDone 回传
    }

    void joinAll() {
        // detach 模式：M4 简化（宿主停机前应等待回调完成）
    }

private:
    std::string host_;
    uint16_t port_;
    std::vector<std::thread> workers_;
};

void destroyAi(void* p) { delete static_cast<AiDrawImpl*>(p); }

// 插件配置键（宿主 config_get）：
//   ai.comfyui.host（默认 127.0.0.1） / ai.comfyui.port（默认 8188）
zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;
    std::string h = "127.0.0.1";
    uint16_t port = 8188;
    const char* v = nullptr;
    if (host->config_get(host->config_ctx, "ai.comfyui.host", "", &v) == ZENITH_OK && v != nullptr) {
        h = v;
    }
    if (host->config_get(host->config_ctx, "ai.comfyui.port", "", &v) == ZENITH_OK && v != nullptr) {
        port = static_cast<uint16_t>(std::atoi(v));
    }
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_comfyui", 0,
                    ("ai_comfyui: init (target " + h + ":" + std::to_string(port) + ")").c_str());

    auto* svc = new (std::nothrow) AiDrawImpl(h, port);
    if (svc == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  zenith::ai::IAiDrawService::service_name(),
                                  zenith::ai::IAiDrawService::service_version(),
                                  static_cast<void*>(svc), &destroyAi);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) { g_host = nullptr; }

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_comfyui",
    "0.1.0",
    "Zenith AI plugin: ComfyUI workflow submission + asset download",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
