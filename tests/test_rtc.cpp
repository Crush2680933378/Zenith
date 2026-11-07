// WebRTC 基础件：STUN 编解码 + 绑定客户端 + host 候选收集
#include <atomic>
#include <chrono>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ztest.hpp"

#include "zenith/rtc/dtls.hpp"
#include "zenith/rtc/stun.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
void closeSock(int fd) {
#if defined(_WIN32)
    closesocket(fd);
#else
    ::close(fd);
#endif
}

/// 极简 STUN 服务端：收到 Binding Request 就回一条 XOR-MAPPED-ADDRESS
class StunTestServer {
public:
    bool start() {
#if defined(_WIN32)
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        fd_ = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port_ = ntohs(addr.sin_port);
#if defined(_WIN32)
        DWORD tv = 100;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_usec = 100 * 1000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        running_.store(true);
        thread_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) {
            closeSock(fd_);
            fd_ = -1;
        }
    }
    ~StunTestServer() { stop(); }
    [[nodiscard]] uint16_t port() const { return port_; }
    [[nodiscard]] int handled() const { return handled_.load(); }

private:
    void loop() {
        char buf[1500];
        while (running_.load()) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            const int n =
                ::recvfrom(fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (n <= 0) continue;
            rtc::StunMessage msg;
            if (!rtc::decode(std::string_view(buf, static_cast<size_t>(n)), msg)) continue;
            if (msg.type != rtc::kStunTypeBindingRequest) continue;

            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            rtc::StunAddress mapped{ip, ntohs(peer.sin_port)};
            const std::string resp = rtc::encodeBindingResponse(mapped, msg.transactionId);
            // 先计数再回包：否则客户端收到响应后立刻断言时，这里可能还没加完（ASan 下必现）
            handled_.fetch_add(1);
            ::sendto(fd_, resp.data(), static_cast<int>(resp.size()), 0,
                     reinterpret_cast<sockaddr*>(&peer), plen);
        }
    }

    int fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> handled_{0};
    std::thread thread_;
};
}  // namespace

/* ------------------------------ 编解码 ------------------------------ */

ZTEST(Stun, ResponseRoundtrip) {
    const uint8_t txn[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    rtc::StunAddress mapped{"192.0.2.1", 32853};
    const std::string wire = rtc::encodeBindingResponse(mapped, txn);

    rtc::StunMessage msg;
    std::string err;
    ZEXPECT_TRUE(rtc::decode(wire, msg, &err));
    ZEXPECT_TRUE(msg.type == rtc::kStunTypeBindingSuccess);
    ZEXPECT_TRUE(msg.xorMapped.has_value());
    ZEXPECT_EQ(msg.xorMapped->ip, std::string("192.0.2.1"));
    ZEXPECT_EQ(msg.xorMapped->port, 32853);
    ZEXPECT_EQ(msg.software, std::string("zenith/rtc"));
    ZEXPECT_EQ(std::memcmp(msg.transactionId, txn, 12), 0);
}

ZTEST(Stun, XorMappedIsActuallyXored) {
    const uint8_t txn[12] = {};
    const rtc::StunAddress mapped{"10.1.2.3", 12345};
    const std::string wire = rtc::encodeBindingResponse(mapped, txn);
    // 明文 IP/端口不应以原样出现在报文里（否则就不是 XOR 了）
    const std::string plainIp = std::string("\x0a\x01\x02\x03", 4);
    const std::string plainPort = std::string("\x30\x39", 2);  // 12345
    ZEXPECT_TRUE(wire.find(plainIp) == std::string::npos);
    ZEXPECT_TRUE(wire.find(plainPort) == std::string::npos);
}

ZTEST(Stun, RejectsMalformed) {
    rtc::StunMessage msg;
    // 头都不够
    ZEXPECT_FALSE(rtc::decode(std::string_view("short", 5), msg));
    // 魔数错
    uint8_t txn[12] = {};
    std::string bad = rtc::encodeBindingRequest(txn);
    bad[4] = 0;
    ZEXPECT_FALSE(rtc::decode(bad, msg));
    // 长度撒谎（声明比实际大）
    std::string truncated = rtc::encodeBindingRequest(txn);
    truncated[3] = static_cast<char>(200);
    ZEXPECT_FALSE(rtc::decode(truncated, msg));
}

ZTEST(Stun, ErrorResponseCarriesCodeAndReason) {
    const uint8_t txn[12] = {9, 9, 9};
    const std::string wire = rtc::encodeBindingError(400, "Bad Request", txn);
    rtc::StunMessage msg;
    ZEXPECT_TRUE(rtc::decode(wire, msg));
    ZEXPECT_TRUE(msg.type == rtc::kStunTypeBindingError);
    ZEXPECT_EQ(msg.errorCode, 400);
    ZEXPECT_EQ(msg.errorReason, std::string("Bad Request"));
}

/* ------------------------------ 端到端 ------------------------------ */

ZTEST(StunE2E, BindingAgainstLoopbackServer) {
    StunTestServer server;
    ZEXPECT_TRUE(server.start());

    rtc::StunAddress mapped;
    std::string err;
    const bool ok = rtc::StunClient::binding("127.0.0.1", server.port(), mapped, 2000, &err);
    ZEXPECT_TRUE(ok);
    if (!ok) {
        std::printf("      [stun] error: %s\n", err.c_str());
    }
    ZEXPECT_EQ(mapped.ip, std::string("127.0.0.1"));
    ZEXPECT_TRUE(mapped.port != 0);
    ZEXPECT_TRUE(server.handled() >= 1);
}

ZTEST(StunE2E, BindingTimesOutAgainstSilentServer) {
    // 绑一个不响应的端口：客户端必须超时返回 false 而不是挂死
    StunTestServer server;
    ZEXPECT_TRUE(server.start());
    const uint16_t silentPort = server.port();
    server.stop();  // 端口关掉 → 没人应答

    rtc::StunAddress mapped;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = rtc::StunClient::binding("127.0.0.1", silentPort, mapped, 200, &err);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    ZEXPECT_FALSE(ok);
    ZEXPECT_TRUE(elapsed < 1500);
}

/* ------------------------------ 候选 ------------------------------ */

ZTEST(Rtc, HostCandidatesAreWellFormed) {
    const auto cands = rtc::gatherHostCandidates();
    for (const auto& c : cands) {
        ZEXPECT_FALSE(c.ip.empty());
        ZEXPECT_EQ(c.type, std::string("host"));
    }
    std::printf("      [rtc] host candidates: %zu\n", cands.size());
}

/* --------------------------- ICE 连通性检查 --------------------------- */

ZTEST(Ice, CheckAttributesRoundtrip) {
    rtc::IceCheckAttrs attrs;
    attrs.username = "remoteU:localU";
    attrs.priority = 1862270975u;
    attrs.useCandidate = true;
    attrs.controlling = true;
    attrs.tieBreaker = 0x1122334455667788ull;
    uint8_t txn[12] = {};
    const std::string wire = rtc::encodeIceCheck(attrs, txn);

    rtc::StunMessage msg;
    ZEXPECT_TRUE(rtc::decode(wire, msg));
    ZEXPECT_TRUE(msg.type == rtc::kStunTypeBindingRequest);
    ZEXPECT_EQ(msg.username, attrs.username);
    ZEXPECT_TRUE(msg.priority.has_value());
    ZEXPECT_EQ(msg.priority.value_or(0), attrs.priority);
    ZEXPECT_TRUE(msg.useCandidate);
    ZEXPECT_TRUE(msg.iceControlling.has_value());
    ZEXPECT_EQ(msg.iceControlling.value_or(0), attrs.tieBreaker);
    ZEXPECT_FALSE(msg.iceControlled.has_value());
}

ZTEST(IceE2E, TwoAgentsConnectOverLoopback) {
    rtc::IceAgent a;
    rtc::IceAgent b;
    ZEXPECT_TRUE(a.bind(0));
    ZEXPECT_TRUE(b.bind(0));
    ZEXPECT_TRUE(a.localPort() != 0);
    ZEXPECT_TRUE(b.localPort() != 0);

    a.addRemoteCandidate({"127.0.0.1", b.localPort()});
    b.addRemoteCandidate({"127.0.0.1", a.localPort()});

    const auto pairsA = a.runChecks(1500);
    const auto pairsB = b.runChecks(1500);
    ZEXPECT_TRUE(!pairsA.empty());
    ZEXPECT_TRUE(!pairsB.empty());

    // 双方都应至少有一条 responded 的对，且收到过带 USE-CANDIDATE 的提名
    ZEXPECT_TRUE(a.nominated() || b.nominated());
    for (const auto& p : pairsA)
        ZEXPECT_EQ(p.remote.ip, std::string("127.0.0.1"));
}

ZTEST(IceE2E, NoCandidateMeansNoPairs) {
    rtc::IceAgent a;
    ZEXPECT_TRUE(a.bind(0));
    const auto pairs = a.runChecks(200);  // 没有远端候选
    ZEXPECT_TRUE(pairs.empty());
    ZEXPECT_FALSE(a.nominated());
}

/* ------------------------------- DTLS ------------------------------- */

#if defined(ZENITH_TLS_OPENSSL)
namespace {
/// 生成自签证书（与 test_tls 同款做法：RSA + SAN=127.0.0.1）
bool makeDtlsCert(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string certPath = dir + "/server.crt";
    const std::string keyPath = dir + "/server.key";

    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (pkey == nullptr) return false;
    X509* x = X509_new();
    if (x == nullptr) {
        EVP_PKEY_free(pkey);
        return false;
    }
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 3600LL * 24);
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
    X509_set_subject_name(x, name);
    X509_set_issuer_name(x, name);
    X509_NAME_free(name);
    const bool signedOk = X509_sign(x, pkey, EVP_sha256()) != 0;

    bool ok = signedOk;
    if (ok) {
        FILE* cf = std::fopen(certPath.c_str(), "wb");
        if (cf == nullptr) {
            ok = false;
        } else {
            ok = PEM_write_X509(cf, x) == 1;
            std::fclose(cf);
        }
    }
    if (ok) {
        FILE* kf = std::fopen(keyPath.c_str(), "wb");
        if (kf == nullptr) {
            ok = false;
        } else {
            ok = PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
            std::fclose(kf);
        }
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
    return ok;
}
}  // namespace

ZTEST(Dtls, HandshakeAndEcho) {
    ZEXPECT_TRUE(rtc::DtlsTransport::supported());
    ZEXPECT_TRUE(makeDtlsCert("zrtc_certs.tmp"));

    rtc::DtlsTransport server;
    ZEXPECT_TRUE(server.listen("zrtc_certs.tmp/server.crt", "zrtc_certs.tmp/server.key", 0));
    const uint16_t port = server.localPort();
    ZEXPECT_TRUE(port != 0);

    std::atomic<bool> accepted{false};
    std::atomic<bool> echoed{false};
    std::thread serverThread([&] {
        if (!server.accept(5000)) return;
        accepted.store(true);
        std::string err;
        const std::string got = server.receive(3000, &err);
        if (!got.empty()) {
            echoed.store(server.send(got));
        }
    });

    rtc::DtlsTransport client;
    ZEXPECT_TRUE(client.connect("127.0.0.1", port, /*verifyPeer=*/false, 5000));
    ZEXPECT_TRUE(client.established());
    ZEXPECT_TRUE(client.send("hello-dtls"));

    std::string err;
    const std::string reply = client.receive(3000, &err);
    serverThread.join();

    ZEXPECT_TRUE(accepted.load());
    ZEXPECT_EQ(reply, std::string("hello-dtls"));

    // 两端导出的密钥材料必须一致（SRTP 就靠这个派生会话密钥）
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;
    if (client.exportKeyingMaterial("EXTRACTOR-dtls_srtp", 32, a) &&
        server.exportKeyingMaterial("EXTRACTOR-dtls_srtp", 32, b)) {
        ZEXPECT_EQ(a.size(), size_t{32});
        ZEXPECT_TRUE(a == b);
    } else {
        ztest::reportFail(__FILE__, __LINE__, "exportKeyingMaterial failed");
    }
}
#else
ZTEST(Dtls, SkippedWithoutOpenSsl) {
    ZEXPECT_FALSE(rtc::DtlsTransport::supported());
}
#endif

ZTEST_MAIN
