# Zenith ESP-IDF 组件端口（M4.6 脚手架）

## 接入方式（ESP32 / ESP32-S3 / ESP32-C3）

```bash
# 你的 ESP-IDF 工程的 CMakeLists.txt：
set(EXTRA_COMPONENT_DIRS "C:/path/to/Zenith/ports/esp-idf")
# 之后组件名 "zenith" 可直接 REQUIRE
idf.py build
```

## 编入范围（M4.6）

- `zenith::core`（Result/Buffer/string/time/KeyValueConfig）
- `zenith::log`（门面 + 控制台，ESP-IDF log 桥接见 log 目录 TODO）
- `zenith::concurrency::ThreadPool`（FreeRTOS 任务包装）
- `zenith::io`（COBS/CRC32 —— 与 edge 网关协议对齐，见 examples/05_esp32_firmware）
- `zenith::plugin`（ServiceRegistry + HostApi——静态注册表替代 dlopen）

## 里程碑说明

- net/http 层的 lwIP 非阻塞适配（socket → lwip_select 轮询）规划于 M4.6 后续；
- 嵌入式插件：ESP-IDF 无动态加载——静态注册（同一 C ABI 签名）：
  ```c
  const zenith_plugin_desc* desc = zenith_plugin_entry(); // 链接期注册进表
  ```
- 泰山派 RK3566/RK3576（aarch64 Linux）：直接用主 CMake + 工具链文件
  `cmake/toolchains/aarch64-linux-gnu.cmake` 交叉构建（epoll 后端）。
