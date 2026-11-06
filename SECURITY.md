# 安全策略

## 报告漏洞

**不要用公开 Issue 报告安全问题。**

请通过 GitHub 的[私密漏洞报告](https://github.com/Crush2680933378/Zenith/security/advisories/new)
提交，或发邮件给维护者（见仓库主页联系方式）。请包含：

- 受影响的版本 / commit
- 复现步骤或 PoC
- 影响面（远程可利用？需要认证？）
- 你希望的署名方式

我们会在 **72 小时内**确认收到，并在修复后公开致谢（除非你要求匿名）。

## 支持范围

| 版本 | 安全更新 |
|---|---|
| v0.2.x | ✅ |
| v0.1.x | ❌ 请升级 |

## 攻击面说明

面向公网部署时，以下组件的输入都当作**不可信**：

| 组件 | 代码位置 | 缓解 |
|---|---|---|
| HTTP/1.1 请求解析 | `src/http/http_parser.cpp` | 模糊测试覆盖；body 上限 `maxBodyBytes` |
| HTTP 响应解析（上游） | 同上 | 同上（客户端视角） |
| WebSocket 帧解码 | `src/net/websocket.cpp` | 模糊测试；opcode/长度/掩码校验 |
| JSON 解析 | `src/json/json.cpp` | 模糊测试；dump→parse 往返校验 |
| WAV 解码（语音上传） | `src/ai/audio_pcm.cpp` | 模糊测试；chunk 长度边界校验 |
| 静态文件服务 | `src/http/static_files.cpp` | 路径穿越防护（`safeJoin`） |
| 插件 `execVerify` | crew 执行闸门 | 命令白名单 + shell 元字符禁用 |

已知限制：

- 默认配置下 **没有内置鉴权**，鉴权由使用方的中间件/网关负责
- TLS 需要显式启用（`-DZENITH_TLS=openssl`），默认明文
- 插件是 **native 代码**，加载即拥有进程权限——只加载可信插件

## 安全相关的 CI

- ASan + UBSan：越界 / UAF / 未定义行为（阻塞）
- TSan：数据竞争（阻塞，并发套件）
- fuzz：五个解析入口，白天冒烟 + 夜间长跑
