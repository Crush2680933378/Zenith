// ============================================================================
//  IDeviceBridge —— 边缘设备桥共享接口（宿主与 edge-esp32 插件之间）
//  协议（COBS+CRC32 帧，JSON 载荷）：
//    设备→桥：{"t":"hello","id":"esp32-01"}   注册
//             {"t":"hb","id":"...","ms":123}  心跳
//             {"t":"resp","rid":7,"ok":true,"data":...}  RPC 应答
//             {"t":"event","id":"...","name":"...","data":...}  事件上报
//    桥→设备：{"t":"rpc","rid":7,"method":"led","args":{"on":true}}
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "zenith/plugin/service.hpp"

namespace zenith::edge {

struct DeviceInfo {
    std::string id;
    std::string transport;   // "tcp" / "serial"
    std::string address;     // ip:port 或 COM 名
    bool online = false;
    int64_t lastSeenMs = 0;  // 最后心跳（宿主单调时钟）
};

class IDeviceBridge : public zenith::plugin::IService {
public:
    ZENITH_SERVICE_DECL(IDeviceBridge, "zenith.edge.IDeviceBridge", 1)

    /// 启动 TCP 网关（设备主动接入）；host="0.0.0.0" 监听全部。返回实际端口。
    virtual int startTcpGateway(const std::string& host, uint16_t port) = 0;

    /// 打开串口设备通道（同帧协议）。可选（无串口环境跳过）。
    virtual bool attachSerial(const std::string& comName, uint32_t baud) = 0;

    /// 设备列表快照
    virtual std::vector<DeviceInfo> devices() = 0;

    /// RPC（异步；回调在线程安全上下文触发；超时毫秒）
    using RpcCallback = std::function<void(bool ok, const std::string& responseJson)>;
    virtual bool sendRpc(const std::string& deviceId, const std::string& method,
                         const std::string& argsJson, RpcCallback cb, int timeoutMs = 4000) = 0;

    /// 事件订阅（hello/hb/event）
    using EventHandler = std::function<void(const std::string& deviceId,
                                            const std::string& eventJson)>;
    virtual void setEventHandler(EventHandler h) = 0;

    virtual void stop() = 0;
};

} // namespace zenith::edge
