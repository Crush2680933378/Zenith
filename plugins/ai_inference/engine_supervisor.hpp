// ============================================================================
//  EngineSupervisor —— 本地推理引擎进程监管（M8.1 重写）
//
//  为什么需要它：本机的引擎是**独立可执行文件**（NInfer 的 ninfer-serve-*.exe、
//  llama.cpp 的 llama-server.exe），能力（三元量化/KVMem/投机解码/视觉）全在
//  进程内、且只从自己的目录加载 CUDA/FFmpeg DLL。进程内链接既拿不到头文件，
//  也拿不到这些服务端特性——所以正确形态是「宿主拉进程 + 说 OpenAI 协议」。
//
//  职责：拉起 → 等就绪（日志标记 or 进程存活）→ 持续读 stdout/stderr →
//        解析控制台统计行 → 崩溃自动重启（有限次）→ 停止时回收。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zenith/ai/inference_service.hpp"

namespace zenith::ai::detail {

struct EngineSpec {
    std::string exe;                                    // 引擎可执行文件全路径
    std::vector<std::string> args;                      // 已展开的参数
    std::vector<std::pair<std::string, std::string>> env; // 追加/覆盖的环境变量
    std::string workDir;                                // 工作目录（引擎加载同目录 DLL）
    std::string readyLine = "listening on";             // 日志就绪标记（空 = 不判日志）
    int startupTimeoutMs = 180000;
    int maxRestarts = 3;                                // 非预期退出后的重启上限
};

class EngineSupervisor {
public:
    EngineSupervisor();
    ~EngineSupervisor();
    EngineSupervisor(const EngineSupervisor&) = delete;
    EngineSupervisor& operator=(const EngineSupervisor&) = delete;

    /// 启动并等待就绪（就绪 = 出现 readyLine，或进程活了 startupTimeoutMs 的 1/3 且未退出）
    bool start(const EngineSpec& spec, std::string& error);
    /// 停止引擎（先温和后强制），可重复调用
    void stop();

    [[nodiscard]] bool running() const;
    /// 运行态快照（含控制台解析出的 TTFT/prefill/decode/投机接受率）
    [[nodiscard]] EngineState snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zenith::ai::detail
