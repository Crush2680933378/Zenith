# 贡献指南

## 环境

- CMake ≥ 3.16、C++17 编译器（GCC 11+ / Clang 14+ / MinGW-w64）
- 可选：OpenSSL（TLS）、zlib（gzip）、whisper.cpp（语音）

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DZENITH_TLS=openssl -DZENITH_ENABLE_ORM=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

子模块（`thirdparty/uORM`）是**可选**的：`-DZENITH_ENABLE_ORM=OFF` 即可绕开。

## 提交前自查

CI 会跑这些，本地先过一遍会快很多：

| 检查 | 本地命令 |
|---|---|
| 单元测试 | `ctest --test-dir build --output-on-failure` |
| Release 模式 | `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && ctest --test-dir build-rel` |
| 内存安全 | `cmake -B build-asan -DZENITH_SANITIZE=address,undefined` + ctest |
| 数据竞争 | `-DZENITH_SANITIZE=thread`，跑 `ctest -R "test_concurrency\|test_m6\|test_websocket\|test_m13b"` |
| 性能回归 | `python3 tools/bench_gate.py --server build/bin/zenith_bench_server --bench build/bin/zenith_bench` |
| 模糊测试 | `-DZENITH_BUILD_FUZZERS=ON`（需 Clang），`./build/fuzz/fuzz_json -max_total_time=60` |
| 代码格式 | `clang-format -i <改动的文件>`（配置见 `.clang-format`） |

## 代码约定

- 缩进 4 空格，行宽 100，`LF` + UTF-8（`.editorconfig` / `.clang-format`）
- 注释用中文，写**为什么**而不是**做了什么**——尤其是踩过的坑要写清"不这么做会怎样"
- 公共头文件只暴露必要接口；内部实现放 `src/`，不要进 `include/zenith/`
- 新增可选依赖必须能**不配置也构建**（参考 whisper 的写法：编译期开关 + 运行期优雅降级）

## 提交信息

沿用现有风格，中文描述，用语义化前缀：

```
feat(m7): 静态文件零拷贝
fix(ws): 拒绝保留 opcode
ci: 新增性能门禁
docs: 更新 M7 进度快照
```

正文要能回答：**改了什么 / 为什么 / 怎么验证的**。修 bug 时写清根因，
而不是只写"修复了问题"。

## 测试新增

- 新功能必须有单测；修 bug 应补一条能复现原问题的回归用例
- 测试用自研 ZTEST 微框架（`tests/ztest.hpp`），**断言宏用值语义**，
  不要把 `obj.fn()->field` 直接塞进 `ZEXPECT_EQ`（会绑到临时对象成员上）
- 涉及时序的断言不要依赖固定 `sleep`，用带超时的轮询

## PR 检查清单

- [ ] `ctest` 全绿（Debug + Release）
- [ ] 新增/修改的行为有测试覆盖
- [ ] 公共 API 变更已更新 README / CHANGELOG
- [ ] 没有把构建产物、测试临时文件提交进仓库
- [ ] 如果改了 CI/构建，确认九个 job 仍然全绿
