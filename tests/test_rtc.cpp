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
#include "zenith/rtc/rtcp.hpp"
#include "zenith/rtc/rtp.hpp"
#include "zenith/rtc/stun.hpp"
#include "zenith/rtc/turn.hpp"
#include "zenith/rtc/udp_mux.hpp"

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

/* -------------------------------- RTP -------------------------------- */

ZTEST(Rtp, HeaderRoundtrip) {
    rtc::RtpPacket pkt;
    pkt.header.payloadType = 111;  // opus
    pkt.header.marker = true;
    pkt.header.sequence = 0xBEEF;
    pkt.header.timestamp = 0x01020304;
    pkt.header.ssrc = 0xDEADBEEF;
    pkt.payload = std::string("\x00\x01\x02payload", 10);

    const std::string wire = rtc::serializeRtp(pkt);
    ZEXPECT_EQ(wire.size(), rtc::kRtpFixedHeaderSize + pkt.payload.size());

    rtc::RtpPacket back;
    std::string err;
    ZEXPECT_TRUE(rtc::parseRtp(wire, back, &err));
    ZEXPECT_EQ(static_cast<int>(back.header.version), 2);
    ZEXPECT_EQ(static_cast<int>(back.header.payloadType), 111);
    ZEXPECT_TRUE(back.header.marker);
    ZEXPECT_EQ(back.header.sequence, 0xBEEF);
    ZEXPECT_EQ(back.header.timestamp, 0x01020304u);
    ZEXPECT_EQ(back.header.ssrc, 0xDEADBEEFu);
    ZEXPECT_EQ(back.payload, pkt.payload);
}

ZTEST(Rtp, CsrcAndExtension) {
    rtc::RtpPacket pkt;
    pkt.header.sequence = 7;
    pkt.header.csrc = {0x11111111u, 0x22222222u};
    pkt.header.extension = true;
    pkt.header.extensionProfile = std::string("\xBE\xDE", 2);
    pkt.header.extensionData = std::string("\x01\x02\x03\x04", 4);  // 1 word
    pkt.payload = "x";

    const std::string wire = rtc::serializeRtp(pkt);
    rtc::RtpPacket back;
    std::string err;
    ZEXPECT_TRUE(rtc::parseRtp(wire, back, &err));
    ZEXPECT_EQ(back.header.csrc.size(), size_t{2});
    ZEXPECT_EQ(back.header.csrc[0], 0x11111111u);
    ZEXPECT_EQ(back.header.csrc[1], 0x22222222u);
    ZEXPECT_TRUE(back.header.extension);
    ZEXPECT_EQ(back.header.extensionProfile, std::string("\xBE\xDE", 2));
    ZEXPECT_EQ(back.header.extensionData, std::string("\x01\x02\x03\x04", 4));
    ZEXPECT_EQ(back.payload, std::string("x"));
}

ZTEST(Rtp, RejectsBadPackets) {
    rtc::RtpPacket p;
    std::string err;
    // 太短
    ZEXPECT_FALSE(rtc::parseRtp(std::string(8, '\0'), p, &err));
    // 版本不是 2
    std::string bad(12, '\0');
    bad[0] = static_cast<char>(1 << 6);  // version=1
    ZEXPECT_FALSE(rtc::parseRtp(bad, p, &err));
    // CSRC 数量超过实际长度
    std::string badCsrc(12, '\0');
    badCsrc[0] = static_cast<char>((2 << 6) | 0x0F);  // version 2, csrc count 15
    ZEXPECT_FALSE(rtc::parseRtp(badCsrc, p, &err));
}

ZTEST(Rtp, PaddingIsStripped) {
    rtc::RtpPacket pkt;
    pkt.header.padding = true;
    pkt.payload = std::string("abcde");
    const std::string wire = rtc::serializeRtp(pkt);

    rtc::RtpPacket back;
    std::string err;
    ZEXPECT_TRUE(rtc::parseRtp(wire, back, &err));
    ZEXPECT_EQ(back.payload, std::string("abcde"));
    ZEXPECT_TRUE(back.header.padding);
}

/* --------------------- WebRTC 栈装配：ICE → DTLS → RTP --------------------- */

#if defined(ZENITH_TLS_OPENSSL)
// 注：ICE 与 DTLS 共用同一条 UDP socket 需要按首字节做 demux
// （STUN 0-3 / DTLS 20-63 / RTP 128-191），当前 IceAgent 的接收线程会把
// 非 STUN 包丢弃。这条路留作后续（见 dtls.hpp 的边界说明）。
ZTEST(WebRtc, KeyingMaterialMatchesAcrossStack) {
    ZEXPECT_TRUE(makeDtlsCert("zrtc_certs.tmp"));
    rtc::DtlsTransport server;
    ZEXPECT_TRUE(server.listen("zrtc_certs.tmp/server.crt", "zrtc_certs.tmp/server.key", 0));
    std::vector<uint8_t> serverKey;
    std::thread th([&] {
        if (server.accept(5000)) server.exportKeyingMaterial("EXTRACTOR-dtls_srtp", 32, serverKey);
    });
    rtc::DtlsTransport client;
    ZEXPECT_TRUE(client.connect("127.0.0.1", server.localPort(), false, 5000));
    th.join();
    std::vector<uint8_t> clientKey;
    ZEXPECT_TRUE(client.exportKeyingMaterial("EXTRACTOR-dtls_srtp", 32, clientKey));
    ZEXPECT_EQ(clientKey.size(), size_t{32});
    ZEXPECT_TRUE(clientKey == serverKey);
}
#endif

/* ----------------------------- UDP demux ----------------------------- */

ZTEST(Mux, ClassifiesByFirstByte) {
    auto kind = [](uint8_t b) {
        const char c = static_cast<char>(b);
        return rtc::classifyDatagram(&c, 1);
    };
    ZEXPECT_TRUE(kind(0x00) == rtc::DatagramKind::Stun);  // Binding Request
    ZEXPECT_TRUE(kind(0x01) == rtc::DatagramKind::Stun);  // Binding Success
    ZEXPECT_TRUE(kind(22) == rtc::DatagramKind::Dtls);    // handshake
    ZEXPECT_TRUE(kind(23) == rtc::DatagramKind::Dtls);    // application data
    ZEXPECT_TRUE(kind(0x80) == rtc::DatagramKind::Rtp);   // RTP v2
    ZEXPECT_TRUE(kind(0x81) == rtc::DatagramKind::Rtp);   // RTP v2 + marker
    // 0x90 属于 0x80..0xBF，是 RTP v2（CC=16），不是未知
    ZEXPECT_TRUE(kind(0x90) == rtc::DatagramKind::Rtp);
    ZEXPECT_TRUE(kind(0x60) == rtc::DatagramKind::Unknown);  // 96：三类之外
}

ZTEST(MuxE2E, RoutesThreeProtocolsOnOneSocket) {
    rtc::UdpMux a;
    rtc::UdpMux b;
    ZEXPECT_TRUE(a.bind(0));
    ZEXPECT_TRUE(b.bind(0));

    std::atomic<int> stunCount{0};
    std::atomic<int> dtlsCount{0};
    std::atomic<int> rtpCount{0};
    std::string lastStun;
    b.onStun([&](std::string_view payload, const rtc::StunAddress&) {
        stunCount.fetch_add(1);
        lastStun.assign(payload);
    });
    b.onDtls([&](std::string_view, const rtc::StunAddress&) { dtlsCount.fetch_add(1); });
    b.onRtp([&](std::string_view, const rtc::StunAddress&) { rtpCount.fetch_add(1); });

    const rtc::StunAddress to{"127.0.0.1", b.localPort()};
    // STUN：用真的 Binding Request
    uint8_t txn[12] = {};
    ZEXPECT_TRUE(a.sendTo(to, rtc::encodeBindingRequest(txn)));
    // DTLS：首字节 22
    ZEXPECT_TRUE(a.sendTo(to, std::string("\x16\xfe\xfdhello", 9)));
    // RTP：首字节 0x80
    ZEXPECT_TRUE(a.sendTo(to, std::string("\x80\x60\x00\x01", 4)));

    for (int i = 0; i < 300 && (stunCount.load() + dtlsCount.load() + rtpCount.load()) < 3; ++i) {
        b.pollOnce(10);
    }
    ZEXPECT_EQ(stunCount.load(), 1);
    ZEXPECT_EQ(dtlsCount.load(), 1);
    ZEXPECT_EQ(rtpCount.load(), 1);
    ZEXPECT_TRUE(lastStun.size() >= 20);  // 至少包含 STUN 头
    ZEXPECT_EQ(b.dropped(), size_t{0});
}

ZTEST(MuxE2E, UnknownBytesAreDropped) {
    rtc::UdpMux a;
    rtc::UdpMux b;
    ZEXPECT_TRUE(a.bind(0));
    ZEXPECT_TRUE(b.bind(0));
    std::atomic<int> hits{0};
    b.onStun([&](std::string_view, const rtc::StunAddress&) { hits.fetch_add(1); });

    ZEXPECT_TRUE(a.sendTo({"127.0.0.1", b.localPort()}, std::string("\x90\x01\x02", 3)));
    for (int i = 0; i < 20; ++i)
        b.pollOnce(10);
    ZEXPECT_EQ(hits.load(), 0);
    ZEXPECT_TRUE(b.dropped() >= 1);
}

/* --------------- ICE + DTLS 共用一条链路（demux + memory BIO） --------------- */

#if defined(ZENITH_TLS_OPENSSL)
ZTEST(WebRtc, StackOnIceSocket) {
    ZEXPECT_TRUE(makeDtlsCert("zrtc_certs.tmp"));

    // 1) ICE 互检，选出链路
    rtc::IceAgent a;
    rtc::IceAgent b;
    ZEXPECT_TRUE(a.bind(0));
    ZEXPECT_TRUE(b.bind(0));
    a.addRemoteCandidate({"127.0.0.1", b.localPort()});
    b.addRemoteCandidate({"127.0.0.1", a.localPort()});
    (void)a.runChecks(1200);
    (void)b.runChecks(1200);
    for (int i = 0; i < 300 && !(a.selectedPair() && b.selectedPair()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ZEXPECT_TRUE(a.selectedPair().has_value());
    ZEXPECT_TRUE(b.selectedPair().has_value());
    const rtc::StunAddress peerA = a.selectedPair()->remote;  // 对端 = B
    const rtc::StunAddress peerB = b.selectedPair()->remote;  // 对端 = A

    // 2) 两端各起一个 memory-BIO 的 DTLS，出站字节走 ICE 的 socket
    rtc::DtlsTransport server;
    rtc::DtlsTransport client;
    ZEXPECT_TRUE(server.initMemory(/*serverSide=*/true, "zrtc_certs.tmp/server.crt",
                                   "zrtc_certs.tmp/server.key",
                                   [&a, peerA](std::string_view d) { a.sendTo(peerA, d); }));
    ZEXPECT_TRUE(client.initMemory(/*serverSide=*/false, {}, {},
                                   [&b, peerB](std::string_view d) { b.sendTo(peerB, d); }));

    // 3) 把非 STUN 数据报喂给各自的 DTLS（这就是 demux 的落地）
    a.setDatagramSink(
        [&server](std::string_view d, const rtc::StunAddress&) { server.feedInbound(d); });
    b.setDatagramSink(
        [&client](std::string_view d, const rtc::StunAddress&) { client.feedInbound(d); });

    // 4) 泵握手
    bool bothUp = false;
    for (int i = 0; i < 400; ++i) {
        const bool s1 = server.step();
        const bool s2 = client.step();
        if (s1 && s2) {
            bothUp = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ZEXPECT_TRUE(bothUp);
    if (!bothUp) return;

    // 5) 在 DTLS 上发一个 RTP 包
    rtc::RtpPacket pkt;
    pkt.header.payloadType = 96;
    pkt.header.sequence = 4242;
    pkt.header.ssrc = 0x11223344;
    pkt.payload = "media-frame";
    ZEXPECT_TRUE(client.send(rtc::serializeRtp(pkt)));

    std::string got;
    for (int i = 0; i < 200 && got.empty(); ++i) {
        (void)server.step();
        std::string err;
        got = server.receive(50, &err);
        if (got.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ZEXPECT_TRUE(!got.empty());
    if (!got.empty()) {
        rtc::RtpPacket parsed;
        ZEXPECT_TRUE(rtc::parseRtp(got, parsed));
        ZEXPECT_EQ(parsed.payload, std::string("media-frame"));
        ZEXPECT_EQ(parsed.header.ssrc, 0x11223344u);
    }
}
#endif

/* -------------------------------- RTCP -------------------------------- */

ZTEST(Rtcp, SenderReportRoundtrip) {
    rtc::RtcpSenderReportPacket sr;
    sr.ssrc = 0xAABBCCDD;
    sr.ntpTimestamp = 0x1122334455667788ull;
    sr.rtpTimestamp = 0x0A0B0C0D;
    sr.packetCount = 100;
    sr.octetCount = 20000;
    rtc::RtcpReportBlock b;
    b.ssrc = 0x01020304;
    b.fractionLost = 12;
    b.cumulativeLost = -3;  // 负值要能原样回读
    b.extendedHighestSeq = 0x00010002;
    b.jitter = 15;
    b.lastSr = 0xCAFEBABE;
    b.delaySinceSr = 7;
    sr.reports.push_back(b);

    const std::string wire = rtc::serializeRtcp(sr);
    std::vector<rtc::RtcpPacket> out;
    size_t consumed = 0;
    std::string err;
    ZEXPECT_TRUE(rtc::parseRtcpCompound(wire, out, consumed, &err));
    ZEXPECT_EQ(consumed, wire.size());
    ZEXPECT_EQ(out.size(), size_t{1});
    ZEXPECT_EQ(static_cast<int>(out[0].type), 200);
    ZEXPECT_EQ(out[0].count, 1);
    ZEXPECT_EQ(out[0].sr.ssrc, sr.ssrc);
    ZEXPECT_EQ(out[0].sr.ntpTimestamp, sr.ntpTimestamp);
    ZEXPECT_EQ(out[0].sr.rtpTimestamp, sr.rtpTimestamp);
    ZEXPECT_EQ(out[0].sr.packetCount, 100u);
    ZEXPECT_EQ(out[0].sr.octetCount, 20000u);
    ZEXPECT_EQ(out[0].sr.reports.size(), size_t{1});
    ZEXPECT_EQ(out[0].sr.reports[0].fractionLost, static_cast<uint8_t>(12));
    ZEXPECT_EQ(out[0].sr.reports[0].cumulativeLost, -3);
    ZEXPECT_EQ(out[0].sr.reports[0].jitter, 15u);
}

ZTEST(Rtcp, CompoundWithCname) {
    const std::string wire = rtc::serializeRtcpCompound(0x01020304, "zenith-cname");
    std::vector<rtc::RtcpPacket> out;
    size_t consumed = 0;
    std::string err;
    ZEXPECT_TRUE(rtc::parseRtcpCompound(wire, out, consumed, &err));
    ZEXPECT_EQ(out.size(), size_t{2});  // RR + SDES
    ZEXPECT_EQ(static_cast<int>(out[0].type), 201);
    ZEXPECT_EQ(static_cast<int>(out[1].type), 202);
    ZEXPECT_EQ(out[1].cname, std::string("zenith-cname"));
}

ZTEST(Rtcp, RejectsMalformed) {
    std::vector<rtc::RtcpPacket> out;
    size_t consumed = 0;
    std::string err;
    // 版本不是 2
    std::string bad(8, '\0');
    bad[0] = 0x40;  // version 1
    ZEXPECT_FALSE(rtc::parseRtcpCompound(bad, out, consumed, &err));
    // 长度撒谎（声明 100 字，实际 8 字节）
    std::string lie(8, '\0');
    lie[0] = static_cast<char>(2 << 6);
    lie[1] = static_cast<char>(200);
    lie[2] = 0;
    lie[3] = 100;
    ZEXPECT_FALSE(rtc::parseRtcpCompound(lie, out, consumed, &err));
    // 未知类型
    std::string unknown(8, '\0');
    unknown[0] = static_cast<char>(2 << 6);
    unknown[1] = static_cast<char>(250);
    unknown[3] = 1;  // 长度 1 字 = 8 字节
    ZEXPECT_FALSE(rtc::parseRtcpCompound(unknown, out, consumed, &err));
}

/* -------------------------------- TURN -------------------------------- */

ZTEST(Turn, RelayRoundtripThroughServer) {
    rtc::TurnServer server;
    ZEXPECT_TRUE(server.listen(0));
    ZEXPECT_TRUE(server.port() != 0);

    // 服务器必须先开始泵包，否则 Allocate 请求没人处理
    std::atomic<bool> stop{false};
    std::thread pump([&] {
        while (!stop.load())
            server.pollOnce(20);
    });

    rtc::TurnClient client;
    ZEXPECT_TRUE(client.allocate("127.0.0.1", server.port(), 2000));
    const rtc::StunAddress relayed = client.relayedAddress();
    ZEXPECT_TRUE(relayed.port != 0);
    ZEXPECT_EQ(relayed.ip, std::string("127.0.0.1"));

    // 一个"对端"普通 UDP socket
    int peerFd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    ZEXPECT_TRUE(peerFd >= 0);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = inet_addr("127.0.0.1");
    ZEXPECT_EQ(::bind(peerFd, reinterpret_cast<sockaddr*>(&local), sizeof(local)), 0);
    socklen_t ll = sizeof(local);
    ::getsockname(peerFd, reinterpret_cast<sockaddr*>(&local), &ll);
    const rtc::StunAddress peerAddr{"127.0.0.1", static_cast<uint16_t>(ntohs(local.sin_port))};

    // 客户端先建权限，再发数据 → 对端应收到
    ZEXPECT_TRUE(client.createPermission(peerAddr, 2000));

    ZEXPECT_TRUE(client.sendTo(peerAddr, "hello-via-relay"));

    char buf[256];
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    DWORD vol = 0;
#if defined(_WIN32)
    DWORD tv = 2000;
    setsockopt(peerFd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = 2;
    setsockopt(peerFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    (void)vol;
    const int n = ::recvfrom(peerFd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    ZEXPECT_TRUE(n > 0);
    if (n > 0) ZEXPECT_EQ(std::string(buf, static_cast<size_t>(n)), std::string("hello-via-relay"));

    // 反向：对端往中继地址发 → 客户端通过 Data indication 收到
    sockaddr_in relaySa{};
    relaySa.sin_family = AF_INET;
    relaySa.sin_port = htons(relayed.port);
    relaySa.sin_addr.s_addr = inet_addr(relayed.ip.c_str());
    ZEXPECT_TRUE(::sendto(peerFd, "reply-from-peer", 15, 0, reinterpret_cast<sockaddr*>(&relaySa),
                          sizeof(relaySa)) == 15);

    rtc::StunAddress gotFrom;
    const std::string got = client.receive(gotFrom, 2000);
    stop.store(true);
    if (pump.joinable()) pump.join();
#if defined(_WIN32)
    closesocket(peerFd);
#else
    ::close(peerFd);
#endif
    ZEXPECT_EQ(got, std::string("reply-from-peer"));
    ZEXPECT_EQ(gotFrom.port, peerAddr.port);
}

ZTEST_MAIN
