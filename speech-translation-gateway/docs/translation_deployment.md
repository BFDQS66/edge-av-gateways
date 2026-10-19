# 本地 LLM 翻译服务

这条链路在 Ubuntu 虚拟机中使用三个本机进程：原有 C++ 语音服务 `127.0.0.1:8765`，llama.cpp 的 Qwen3 模型服务 `127.0.0.1:8766`，以及 C++ 翻译网关 `127.0.0.1:8767`。翻译模型故障不会使 STT/TTS 进程退出。三个端口都只监听环回地址；远程使用 SSH 转发。

```text
WAV -> SenseVoice STT -> Qwen3 翻译 -> Piper TTS -> WAV
           :8765             :8766        :8765
                 C++ 翻译网关 :8767
```

## 固定版本与安装

- llama.cpp 源码提交：`711780ffc75bbc5645173ed8647a7b3f328c052a`；源码归档 SHA-256：`9aeb03d2df848342fbf194fa78a5fd04ac6345e4d66b4d4bfcd078d5946c77fd`。
- 模型：[ggml-org/Qwen3-1.7B-GGUF](https://huggingface.co/ggml-org/Qwen3-1.7B-GGUF/blob/daeb8e2d528a760970442092f6bf1e55c3b659eb/Qwen3-1.7B-Q4_K_M.gguf)，提交 `daeb8e2d528a760970442092f6bf1e55c3b659eb`，Q4_K_M，1,282,439,264 字节，SHA-256：`d2387ca2dbfee2ffabce7120d3770dadca0b293052bc2f0e138fdc940d9bc7b5`。上游标注 Apache-2.0。
- 模型下载脚本使用 ModelScope 的同名镜像分段获取，但完成后必须匹配上述 Hugging Face 提交的 SHA-256；镜像更新或文件不同时拒绝安装。
- 虚拟机已有 CMake、GCC、Python 3、`libjsoncpp-dev`、sherpa-onnx 和 cpp-httplib。迁移到新系统时需先补齐这些编译依赖。模型与 llama.cpp 源码保存在本项目的 `models/`、`deps/`，不进入 PetaLinux SDK。

在项目根目录执行：

```bash
bash scripts/install_llm_runtime.sh
python3 scripts/download_llm_model.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSHERPA_ONNX_ROOT="$PWD/deps/sherpa-onnx-v1.13.8-linux-x64-shared-lib"
cmake --build build -j2 --target translation_server
bash scripts/deploy_translation_service.sh
```

服务由 `llama-translate.service`、`translation-gateway.service` 管理；`translation-health.timer` 每分钟探测上游并在连续失败后重启。可检查：

```bash
systemctl --user status llama-translate.service translation-gateway.service
systemctl --user status translation-health.timer
journalctl --user -u llama-translate.service -n 30 --no-pager
```

## 接口

`POST http://127.0.0.1:8767/v1/translate`：JSON 输入 `{"text":"你好","source":"zh","target":"en"}`，返回译文、翻译耗时和 token 数。仅支持 `zh -> en`、`en -> zh`，文本最多 1000 UTF-8 字节。`GET /healthz` 只检查网关进程，`GET /readyz` 同时检查语音与模型上游，`GET /metrics` 提供请求计数。

`POST /v1/speech-translate?source=zh&target=en`：请求体为原有语音服务支持的 PCM16 单声道 WAV，返回原文和译文。源语言必须由调用方显式指定；当前没有在网关中验证语种识别结果。`scripts/voice_translate.py` 继续把译文发给原有 TTS，并保存目标语言 WAV 和 JSON 记录：

```bash
python3 scripts/voice_translate.py \
  --wav models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/test_wavs/zh.wav \
  --source zh --target en
python3 scripts/evaluate_translation.py
```

翻译网关最多同时执行一条翻译链路；忙时快速返回 503。llama.cpp 设为单槽、2 CPU 线程、2048 token 上下文和 256 输出 token；单条请求最长等待 120 秒。数字、姓名、地名和否定词仍需评测和人工核对；模型可能翻译错误，不得把输出当作确认过的事实。虚拟机和 RK3588S 的推理延迟、内存占用不同，上板后需要重新测。
