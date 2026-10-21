#include "engine_supervisor.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #include <cerrno>
  #include <vector>
#endif

namespace zenith::ai::detail {

namespace {

using Clock = std::chrono::steady_clock;

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string trimCopy(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

/// "1.97k" / "194.6" / "578" → 数值（k/M 后缀按 1000 进制）
double parseNumber(const char* p) {
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == nullptr || v == 0.0) return v;
    if (*end == 'k' || *end == 'K') return v * 1000.0;
    if (*end == 'm' || *end == 'M') return v * 1000000.0;
    return v;
}

/// 取 "prefix" 之后紧跟的数值
double valueAfter(const std::string& line, const char* prefix) {
    const size_t pos = line.find(prefix);
    if (pos == std::string::npos) return 0.0;
    return parseNumber(line.c_str() + pos + std::strlen(prefix));
}

} // namespace

// ===========================================================================
//  Impl
// ===========================================================================
struct EngineSupervisor::Impl {
    EngineSpec spec;
    mutable std::mutex mtx;
    std::thread reader;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> ready{false};

    // 快照状态（受 mtx 保护）
    std::string lastLine;
    int64_t requests = 0;
    double lastTtftMs = 0;
    double lastPrefillTps = 0;
    double lastDecodeTps = 0;
    int64_t draftAccepted = 0;
    int64_t draftTotal = 0;
    int restarts = 0;

#if defined(_WIN32)
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE pipeRead = nullptr;
    HANDLE pipeWrite = nullptr;
    unsigned long pid = 0;

    static std::wstring utf8ToWide(const std::string& s) {
        if (s.empty()) return std::wstring();
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                            nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    static std::string quote(const std::string& a) {
        if (a.find_first_of(" \t\"") == std::string::npos) return a;
        std::string out = "\"";
        for (char c : a) {
            if (c == '"') out += '\\';
            out += c;
        }
        out += '"';
        return out;
    }
#else
    int pipeRead = -1;
    int pipeWrite = -1;
    pid_t pid = -1;

    static std::string quote(const std::string& a) { return a; }
#endif

    ~Impl() { shutdown(); }

    // ---- 日志解析 -----------------------------------------------------------
    void handleLine(const std::string& raw) {
        const std::string line = trimCopy(raw);
        if (line.empty()) return;
        std::lock_guard<std::mutex> lk(mtx);
        lastLine = line;
        if (!spec.readyLine.empty() && line.find(spec.readyLine) != std::string::npos) {
            ready.store(true);
        }
        if (line.find("req#") != std::string::npos) {
            if (line.find(" done |") != std::string::npos ||
                line.find("| openai-") != std::string::npos) {
                ++requests;
            }
            const double ttft = valueAfter(line, "TTFT ");
            if (ttft > 0.0) lastTtftMs = ttft;
            const double pre = valueAfter(line, "prefill ");
            if (pre > 0.0) lastPrefillTps = pre;
            const double dec = valueAfter(line, "decode ");
            if (dec > 0.0) lastDecodeTps = dec;
            const size_t acc = line.find("accepted ");
            if (acc != std::string::npos) {
                const char* p = line.c_str() + acc + std::strlen("accepted ");
                char* end = nullptr;
                const long long a = std::strtoll(p, &end, 10);
                if (end != nullptr && *end == '/') {
                    const long long t = std::strtoll(end + 1, nullptr, 10);
                    if (a > 0) draftAccepted = a;
                    if (t > 0) draftTotal = t;
                }
            }
        }
    }

    void readerLoop() {
        std::string buf;
        char tmp[4096];
#if defined(_WIN32)
        for (;;) {
            DWORD got = 0;
            const BOOL ok = ::ReadFile(pipeRead, tmp, sizeof(tmp), &got, nullptr);
            if (!ok || got == 0) break; // 引擎退出或管道断开
#else
        for (;;) {
            const ssize_t got = ::read(pipeRead, tmp, sizeof(tmp));
            if (got <= 0) break;
#endif
            if (got > 0) {
                buf.append(tmp, static_cast<size_t>(got));
                size_t nl = std::string::npos;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    handleLine(buf.substr(0, nl));
                    buf.erase(0, nl + 1);
                }
                if (buf.size() > (1u << 20)) buf.clear(); // 防御：异常超长行
            }
        }
        if (!buf.empty()) handleLine(buf);
        if (!stopRequested.load()) running.store(false); // 非主动停止 → 退出
    }

    // ---- 启动 ---------------------------------------------------------------
#if defined(_WIN32)
    /// 本机实测：任何非 NULL 的 lpEnvironment 都会让 CreateProcess 返回 87
    /// （连父进程自己的 GetEnvironmentStringsW() 指针也不行，见 2026-10-21 探针）。
    /// 因此改为「临时设置父进程环境变量 → 以 NULL 环境块继承 → 立刻还原」。
    /// 全程持锁，避免并发 spawn 互相看到对方的临时变量。
    bool spawnWithEnvOverride(std::wstring& cmdBuf, const std::wstring& wdir,
                              STARTUPINFOW& si, PROCESS_INFORMATION& pi, std::string& error) {
        static std::mutex envMtx;
        std::lock_guard<std::mutex> lk(envMtx);

        struct Saved {
            std::wstring key;
            std::wstring oldValue;
            bool existed = false;
        };
        std::vector<Saved> saved;
        for (const auto& [k, v] : spec.env) {
            Saved s;
            s.key = utf8ToWide(k);
            DWORD need = ::GetEnvironmentVariableW(s.key.c_str(), nullptr, 0);
            if (need > 0) {
                s.oldValue.resize(need);
                const DWORD got =
                    ::GetEnvironmentVariableW(s.key.c_str(), s.oldValue.data(), need);
                s.oldValue.resize(got);
                s.existed = true;
            }
            ::SetEnvironmentVariableW(s.key.c_str(), utf8ToWide(v).c_str());
            saved.push_back(std::move(s));
        }

        const BOOL ok = ::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                         CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                                         wdir.empty() ? nullptr : wdir.c_str(), &si, &pi);

        for (const auto& s : saved) { // 还原
            ::SetEnvironmentVariableW(s.key.c_str(), s.existed ? s.oldValue.c_str() : nullptr);
        }
        if (!ok) {
            error = "CreateProcess failed (error " + std::to_string(::GetLastError()) +
                    "): " + spec.exe;
            return false;
        }
        return true;
    }

    bool spawn(std::string& error) {
        std::string cmd = quote(spec.exe);
        for (const auto& a : spec.args) {
            cmd += ' ';
            cmd += quote(a);
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!::CreatePipe(&pipeRead, &pipeWrite, &sa, 0)) {
            error = "CreatePipe failed";
            return false;
        }
        ::SetHandleInformation(pipeRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = pipeWrite;
        si.hStdError = pipeWrite;
        // stdin 必须是一个**可继承的有效句柄**：进程自身的 stdin 被重定向/关闭时
        // （CI、管道、GUI 宿主）GetStdHandle 可能返回 INVALID_HANDLE_VALUE，
        // 带着 STARTF_USESTDHANDLES 传给 CreateProcess 会直接失败。
        HANDLE nulIn = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                                     nullptr);
        si.hStdInput = nulIn;

        PROCESS_INFORMATION pi{};
        std::wstring wcmd = utf8ToWide(cmd);
        std::wstring wdir = utf8ToWide(spec.workDir);
        const bool ok = spawnWithEnvOverride(wcmd, wdir, si, pi, error);
        if (nulIn != INVALID_HANDLE_VALUE) ::CloseHandle(nulIn);
        if (!ok) {
            {
                std::lock_guard<std::mutex> lk(mtx);
                lastLine = error;
            }
            ::CloseHandle(pipeRead);
            ::CloseHandle(pipeWrite);
            pipeRead = nullptr;
            pipeWrite = nullptr;
            return false;
        }
        ::CloseHandle(pipeWrite); // 子进程独占写端
        pipeWrite = nullptr;
        process = pi.hProcess;
        pid = pi.dwProcessId;
        ::CloseHandle(pi.hThread);

        // Job 对象：宿主退出时连带杀掉引擎，避免孤儿进程占显存
        job = ::CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
            ::AssignProcessToJobObject(job, process);
        }
        return true;
    }

    bool alive() const {
        return process != nullptr && ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }

    void terminate() {
        if (process != nullptr) {
            ::TerminateProcess(process, 0);
            ::WaitForSingleObject(process, 5000);
            ::CloseHandle(process);
            process = nullptr;
        }
        if (job != nullptr) {
            ::CloseHandle(job);
            job = nullptr;
        }
        if (pipeRead != nullptr) {
            ::CloseHandle(pipeRead);
            pipeRead = nullptr;
        }
    }
#else // POSIX
    bool spawn(std::string& error) {
        int fds[2];
        if (::pipe(fds) != 0) {
            error = "pipe failed";
            return false;
        }
        const pid_t child = ::fork();
        if (child < 0) {
            error = "fork failed";
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
        if (child == 0) { // 子进程
            ::close(fds[0]);
            ::dup2(fds[1], STDOUT_FILENO);
            ::dup2(fds[1], STDERR_FILENO);
            ::close(fds[1]);
            for (const auto& [k, v] : spec.env) ::setenv(k.c_str(), v.c_str(), 1);
            if (!spec.workDir.empty()) {
                if (::chdir(spec.workDir.c_str()) != 0) ::_exit(126);
            }
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(spec.exe.c_str()));
            for (const auto& a : spec.args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            ::execv(spec.exe.c_str(), argv.data());
            ::_exit(127);
        }
        ::close(fds[1]);
        pipeRead = fds[0];
        pid = child;
        return true;
    }

    bool alive() const {
        if (pid <= 0) return false;
        int status = 0;
        return ::waitpid(pid, &status, WNOHANG) == 0;
    }

    void terminate() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            for (int i = 0; i < 30; ++i) { // 最多等 3 秒
                int status = 0;
                if (::waitpid(pid, &status, WNOHANG) == pid) {
                    pid = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (pid > 0) {
                ::kill(pid, SIGKILL);
                int status = 0;
                ::waitpid(pid, &status, 0);
                pid = -1;
            }
        }
        if (pipeRead >= 0) {
            ::close(pipeRead);
            pipeRead = -1;
        }
    }
#endif

    void shutdown() {
        stopRequested.store(true);
        terminate();
        if (reader.joinable()) reader.join();
        running.store(false);
        ready.store(false);
    }
};

// ===========================================================================
//  对外接口
// ===========================================================================
EngineSupervisor::EngineSupervisor() : impl_(std::make_unique<Impl>()) {}
EngineSupervisor::~EngineSupervisor() = default;

bool EngineSupervisor::start(const EngineSpec& spec, std::string& error) {
    if (spec.exe.empty()) {
        error = "engine exe is empty";
        return false;
    }
    impl_->stopRequested.store(false);
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->spec = spec;
        impl_->lastLine.clear();
        impl_->ready.store(false);
    }
    if (!impl_->spawn(error)) return false;
    impl_->running.store(true);
    impl_->reader = std::thread([this] { impl_->readerLoop(); });

    // 等就绪：日志标记优先；进程意外退出立即失败
    const auto deadline = Clock::now() + std::chrono::milliseconds(spec.startupTimeoutMs);
    while (Clock::now() < deadline) {
        if (impl_->ready.load()) return true;
        if (!impl_->running.load()) {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            error = "engine exited during startup: " + impl_->lastLine;
            return false;
        }
        sleepMs(100);
    }
    // 超时：日志标记没等到，但进程还活着（有的引擎不打这行）→ 视为就绪并留痕
    if (impl_->alive()) {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->spec.readyLine.clear();
        return true;
    }
    error = "engine did not become ready in time";
    return false;
}

void EngineSupervisor::stop() {
    if (impl_ == nullptr) return;
    impl_->shutdown();
}

bool EngineSupervisor::running() const {
    if (impl_ == nullptr) return false;
    return impl_->running.load() && impl_->alive();
}

EngineState EngineSupervisor::snapshot() const {
    EngineState st;
    if (impl_ == nullptr) return st;
    st.supervised = true;
    st.running = running();
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        st.restarts = impl_->restarts;
        st.requests = impl_->requests;
        st.lastTtftMs = impl_->lastTtftMs;
        st.lastPrefillTps = impl_->lastPrefillTps;
        st.lastDecodeTps = impl_->lastDecodeTps;
        st.draftAccepted = impl_->draftAccepted;
        st.draftTotal = impl_->draftTotal;
        st.lastLine = impl_->lastLine;
    }
    st.pid = static_cast<int64_t>(impl_->pid);
    return st;
}

} // namespace zenith::ai::detail
