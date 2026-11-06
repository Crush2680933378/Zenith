# 语音服务（STT / TTS）

`ai_speech` 插件对外注册两个服务：

| 服务名 | 接口 | 版本 |
|---|---|---|
| `zenith.ai.stt` | `ai::ISpeechToText` | 1 |
| `zenith.ai.tts` | `ai::ITextToSpeech` | 1 |

上层（网关 / crew / agent）只依赖抽象，不关心后端是 mock 还是真模型。

---

## 1. 后端矩阵

| 后端 | STT | TTS | 依赖 | 用途 |
|---|---|---|---|---|
| `mock`（默认） | 固定文本回显 | 可播放 440Hz 正弦 WAV | 无 | 联调 / 单测 / 演示 |
| `whisper` | whisper.cpp 真转写 | —（仍用 mock） | whisper.cpp + ggml 模型 | 生产 STT |

后端通过配置选择，**默认 `mock`**，因此不装任何东西也能跑通全链路。

```json
{
  "ai": {
    "speech": {
      "backend": "whisper",
      "whisper": {
        "model": "models/ggml-base.bin",
        "language": "zh",
        "threads": 4
      }
    }
  }
}
```

- `whisper.model`（必填）：ggml / gguf 模型路径
- `whisper.language`：`auto`（默认，自动识别）/ `zh` / `en` / …
- `whisper.threads`：解码线程数，默认 4

模型加载失败、文件不存在、未编入 whisper —— 三种情况都**不会**让插件初始化失败：
记录 warn 日志（含原因）并回落到 `mock`，服务照常可用。

---

## 2. 编入 whisper.cpp

默认构建不依赖 whisper。需要真 STT 时：

```bash
# 1) 准备 whisper.cpp（含 whisper.h 与 libwhisper）
git clone https://github.com/ggerganov/whisper.cpp
cd whisper.cpp && cmake -B build && cmake --build build -j
cd ..

# 2) 带 whisper 构建 Zenith
cmake -B build -DZENITH_WHISPER_ROOT=/path/to/whisper.cpp
cmake --build build -j
```

CMake 会在 `ZENITH_WHISPER_ROOT` 下按常见布局找 `whisper.h` 与 `libwhisper`
（根目录 / `include` / `build/src` / `lib` 等）。找到才定义 `ZENITH_HAS_WHISPER=1`；
没找到只打印一条 STATUS，继续按「不可用占位」编译——所以配错路径不会让构建失败，
但运行时会得到明确原因：

```
whisper: not compiled in — configure with -DZENITH_WHISPER_ROOT=<whisper.cpp root>
```

自检：

```cpp
ai::whisperCompiledIn();   // 本次构建是否编入
ai::whisperLastError();    // 最近一次构造失败的原因
```

---

## 3. 音频输入

`transcribe(audio, format)` 接受：

| format | 说明 |
|---|---|
| `wav` | RIFF/WAVE：PCM 8/16/24/32bit 与 IEEE float32；任意采样率、单/多声道 |
| `pcm16` | 裸 int16 小端，约定 16kHz 单声道（电话 / 流式场景） |
| `""`（空） | 按 RIFF 魔数自动判定，非 RIFF 按 `pcm16` |

内部统一处理成 whisper 唯一接受的形态：**16kHz 单声道 float32**。

- 多声道：各声道取算术平均降混（语音足够，不做相位处理）
- 重采样：线性插值到 16kHz
- 8bit WAV 是无符号偏置编码，已按 `(v-128)/128` 还原
- 少于 100ms 的音频直接拒绝（whisper 自身会报错，这里给出人话）

前处理在 `ai::decodeAudioTo16kMono()` 里，是纯函数、零依赖，可单独复用与单测。

> 采样精度说明：线性重采样对 16k 目标足够（语音频带 8kHz 以内）。
> 若后续接入 48k 高保真场景，再考虑加窗 sinc 重采样。

---

## 4. 并发与资源

- 一个 `whisper_context` 常驻，**转写调用串行化**（whisper 上下文非线程安全）
- 上下文按需在插件 `init` 时加载：模型越大首响越慢（base ~百毫秒级，large 数秒）
- 进程退出时由 `whisper_free` 释放（RAII，插件 shutdown 即释放）

---

## 5. 测试

```bash
ctest -R test_m8_speech --output-on-failure
```

覆盖：WAV 头解析、幅度/时长、立体声降混、48k→16k 重采样、裸 pcm16、垃圾输入拒绝、
whisper 不可用时的降级语义（返回 nullptr + 可读原因，不崩不抛）。

真模型转写的端到端验证需要模型文件，CI 不跑；本地：

```bash
ZENITH_WHISPER_MODEL=models/ggml-base.bin ./bin/your_app
```
