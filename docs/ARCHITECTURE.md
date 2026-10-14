# Zenith 架构说明

## 1. 分层与依赖方向

```
zenith/abi      纯 C 插件 ABI（零依赖；C89/C99/C++、任意主流编译器可编译）
    ↑
zenith::core    Result / Buffer / string / time / KeyValueConfig / 导出宏
    ↑
zenith::log     LogSink 抽象 + ConsoleLogger + AsyncFileLogger(MPSC)
    ↑
zenith::concurrency  ThreadPool + MpscRingQueue（Vyukov 无锁有界队列）
    ↑
zenith::plugin  DynLib → ServiceRegistry → HostApi(C thunk) → PluginManager
    ↑
(M2) zenith::net(EventLoop×4后端) → zenith::http → 路由/中间件
(M3) zenith::db(基于 uORM 服务化) / websocket / redis(RESP 自研)
(M4) tls 后端实装 / media-ffmpeg / ai-comfyui / edge-esp32 / RK 板卡

zenith::coro    可选层：C++20 Task<T>（编译开关 ZENITH_ENABLE_COROUTINES，
                核心库保持 C++17，互不侵入）
thirdparty/uORM git submodule（ZENITH_ENABLE_ORM，默认只构建 uORM::uorm 核心）
```

依赖只允许自下而上单向引用；同层模块互不引用。

## 2. ABI 红线（最重要的一节）

**跨动态库边界只允许出现 C 类型。** 任何 STL 类型（std::string/std::vector/…）、
C++ 虚函数、模板符号都不得穿越 `.dll`/`.so` 边界——不同编译器（甚至同编译器
不同版本）对这些东西的 ABI 不稳定。ESP32 的 xtensa-gcc、泰山派的 aarch64-gcc、
桌面的 MSVC 要能共同对话，唯一公共语言是 C ABI。

两条合法通道：

1. **纯 C ABI**（`zenith/abi/zenith_abi.h`）：任何编译器写的插件与宿主互通。
   服务以 `void*` + 函数表（vtable 结构体）注册；字符串用 `const char*`。
2. **C++ 虚接口**（`zenith/plugin/service.hpp` 的 IService 体系）：仅限宿主与
   插件**用同一工具链/同一运行库构建**时使用（典型：同仓库内一起编译的模块、
   同一台 CI 产出的产物）。Zenith 自身仓库内的插件属于此类。

前向兼容规则：
- 所有 ABI 结构体以 `struct_size` + `abi_version` 开头；新版本只能向**尾部追加**
  字段；读方遇到更大的 `struct_size` 忽略未知尾部，遇到未知函数指针必须判空。
- 插件只导出一个符号 `zenith_plugin_entry`；宿主校验 `abi_version <= ZENITH_ABI_VERSION`。

## 3. 插件生命周期与安全卸载

```
load → init(host) → start() → [运行期] → stop() → shutdown() → 卸载(dlciose)
```

- `init` 收到 `zenith_host_api` 表：日志 / 内存 / 服务注册·查询 / 配置 / 线程池
  派发，全部是函数指针 + 各自不透明 ctx。表中 `plugin_token` 是宿主注入的身份
  令牌，注册服务时必须回传。
- 该表**存储在宿主的 PluginInfo 中**（堆上，加载期间稳定）——插件可以缓存它
  直到 `shutdown` 返回。（历史上曾把它放在栈上传出导致悬空指针，见测试
  `PluginManager.LoadStartQueryUnload` 防回归。）
- 卸载安全序（`PluginManager::unloadAll` 逐插件**逆序**执行）：
  `stop → registry.removeByOwner(token)（调用插件 destroy，此刻代码仍在内存）
  → shutdown → dlclose`。绝不颠倒：dlclose 之后再调 destroy 是跳空指针。

## 4. 服务注册表语义

- 键 = 接口名字符串 + 实现版本；查询方给 `min_version`，低于即
  `ZENITH_ERR_VERSION_MISMATCH`，未注册 `ZENITH_ERR_NOT_FOUND`。
- typed 条目：宿主侧 `shared_ptr` 托管（`put<T>/get<T>`）。
- raw 条目：插件经 C ABI 注册的 `void*` + 可选 destroy + owner 令牌。
  `queryRaw` 返回裸指针，**不转移所有权**，宿主须在使用完毕且卸载插件前查询。
- 覆盖语义：默认 `ALREADY_EXISTS`；显式 replace 时旧 raw 条目先 destroy。

## 5. Windows 动态库的三个坑（都已固化到代码/测试）

1. **静态/动态双构建**：`ZENITH_API` 三态宏（dllexport / 空 / dllimport），
   `zenith_core` 目标 PUBLIC 注入 `ZENITH_STATIC`。消费静态库无需手动定义。
2. **退出期加载器锁死锁**：异步 sink 的后台线程若在 `DLL_PROCESS_DETACH`
   阶段 join，会与线程退出的 `DLL_THREAD_DETACH` 互锁，进程永远退不出去。
   规则：**宿主必须在 main 返回前调用 `LoggerFacade::shutdown()`**（示例已示范）。
3. 大对象勿放栈上：AsyncFileLogger 的 65536 槽队列（约 2.6MB）已改为堆持有。

## 6. 协程层（可选编译）

- `ZENITH_ENABLE_COROUTINES=ON` 时新增 `zenith_coro` 接口层并切换该层到 C++20；
  核心/测试默认路径不受影响（C++17 承诺不破）。
- `Task<T>`：惰性启动、对称转移（final_suspend 直接切回等待者）、异常在
  co_await 处重抛。顶层用 `syncWait`；M2 的 EventLoop 就位后提供
  `loop.run(task)`，I/O awaiter 在事件回调中 `handle.resume()`。
- 所有异步 API 提供双形态：回调/Future（C++17 通用）与 co_await（开启协程后）。

## 7. 日志

- 写路径：宏（变参模板折叠拼接，无 printf 格式串风险）→ 门面（级别过滤）
  → 各 sink。控制台 sink 自动探测 VT100/ANSI；文件 sink 为 MPSC 无锁队列 +
  后台线程 + 尺寸滚动。队列满丢弃并计数（`droppedCount()`），不阻塞业务线程。
- 注意：直接调 `LoggerFacade::write` 也会做级别过滤（与宏路径一致）。

## 8. uORM 集成方式

- git submodule 于 `thirdparty/uORM`（内部再嵌 uJSON）。上游演进直接在 uORM
  仓库扩展（如新增 Redis 驱动），Zenith 侧只认 `uORM::uorm` 目标与后续在
  M3 定义的 `zenith::db` 抽象。
- 构建只开核心：`UORM_ENABLE_WEB/UORM_BUILD_C_ABI/UORM_BUILD_UORMPP/示例/测试`
  全部关闭（其 asio Web 控制台不进入 Zenith 构建；M3 后可反向迁移到 Zenith
  自研 HTTP 服务器承载）。
- 驱动（libmariadb/libpq/sqlite3）按环境自动探测，缺失优雅降级；
  `ZENITH_ENABLE_ORM=OFF` 时整个仓库无 uORM 依赖（已验证）。

## 9. 测试体系

自研 ZTEST（`tests/ztest.hpp`）：`ZTEST(套名, 用例名) + ZEXPECT_* + ZTEST_MAIN`，
逐可执行注册 ctest。覆盖：core 工具、无锁队列多生产者无丢失无重复、线程池
排空/幂等关闭、日志格式与滚动、注册表版本协商/owner 清理、宿主 API thunk、
**真实加载两个示例插件**的端到端（含纯 C 插件）。

## 10. 嵌入式路线（M4）

- ESP32：ESP-IDF 以 component 形式接入 `zenith::core/log/plugin`（纯 C ABI +
  select 降级 EventLoop + lwIP 适配）；插件系统在 ESP32 上以静态注册表替代
  dlopen（同一 ABI，宿主实现切换）。
- 泰山派 RK3566/RK3576：aarch64-linux-gnu 工具链（`cmake/toolchains/`），
  TLS 走 mbedTLS/wolfSSL，edge 插件承载串口/外设桥。
- FFmpeg / ComfyUI：均为普通 C ABI 插件，宿主经 HTTP（M2 客户端）或进程间
  管道与其交互，核心零依赖。
