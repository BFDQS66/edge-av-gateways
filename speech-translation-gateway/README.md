# 离线语音翻译工程（Ubuntu VM + Orange Pi 5 Pro）

这是离线语音翻译工程：C++ 服务提供 STT、VAD、中英文 TTS 和本地 LLM 翻译，systemd 管理常驻进程与健康探测。先在 Ubuntu x86_64 虚拟机建立基线，现已部署到 Orange Pi 5 Pro（RK3588S、16GB）并完成 ARM 端测试。所有内容位于独立工作目录，不依赖 PetaLinux SDK。板端步骤与实测见 [板端部署](docs/board_deployment.md)。

## 已部署

| 功能 | 模型与运行方式 | 用途 |
| --- | --- | --- |
| STT | SenseVoice Small INT8，sherpa-onnx C API | 中英文离线识别 |
| VAD | Silero VAD | 静音过滤、语音切段 |
| 实时 TTS | Kokoro 多语言 INT8，独立 C++ 服务 | 中英文耳机播报 |
| TTS 速度基线 | Piper 中文 chaowen medium、英文 lessac medium | 较低延迟的中英文合成 |
| LLM 翻译 | Qwen3 1.7B Q4_K_M、llama.cpp CPU 服务 | 中英双向本地翻译 |
| 翻译网关 | C++、cpp-httplib、jsoncpp | 文本翻译和语音识别后翻译 |

模型推理、VAD、WAV 输入输出与服务端性能计时在 C++ 中实现。板端持续收音用 Python 管理 ALSA/ffmpeg 管道、按音量切句、提供本地页面；Python 也用于评测、压力测试和健康探测。

`src/speech_server.cpp` 是常驻服务：启动时加载 SenseVoice、Silero VAD 和中英文 Piper，使用内存中的 WAV 读写。它默认只监听 `127.0.0.1:8765`，并发推理忙时返回 HTTP 503。接口、资源限制和运维命令见 [服务运维](docs/operations.md)。

`src/quality_tts_server.cpp` 单独加载 Kokoro INT8，在 `127.0.0.1:8768` 合成中文和英文。语音合成与识别在不同进程和队列中运行，较慢的播报不会阻塞下一段识别。Piper 保留为速度基线。

`src/translation_server.cpp` 在 `127.0.0.1:8767` 提供翻译接口，调用本机 `127.0.0.1:8766` 的 llama.cpp。翻译与语音服务独立运行，避免 LLM 故障使 STT/TTS 退出。安装见 [LLM 部署](docs/translation_deployment.md)，实测见 [翻译结果](docs/translation_results.md)。

板端持续收音界面已部署：`live-translate.service` 在内存中按句切分麦克风输入，调用 C++ 网关做中英双向识别与翻译，再调用 Kokoro TTS 从耳机播放译文。Chromium 在板载桌面打开 `http://127.0.0.1:8770/`，页面可切换方向、关闭播报和试听耳机。关闭页面不会停止监听。采集、推理、合成和播放使用不同线程，等待队列有上限；异常后自动重连并有健康定时器。Windows 的 `scripts/collect_live_board.ps1` 把每条结果与错误保存在 E 盘，详情见 [板端部署](docs/board_deployment.md)。

## 虚拟机路径和运行

虚拟机项目：`/home/fu-shanfeng/workspace/speech-translation-gateway`。以下命令在该目录执行，需有 `cmake`、C++17 编译器、`wget`、`tar`、`sha256sum` 与 Python 3：

```bash
bash scripts/install_vm_models.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSHERPA_ONNX_ROOT="$PWD/deps/sherpa-onnx-v1.13.8-linux-x64-shared-lib"
cmake --build build -j2
bash scripts/deploy_user_service.sh
```

下载脚本固定 sherpa-onnx `v1.13.8` 和各模型归档的 SHA-256；依赖、模型及测试输出分别放在 `deps/`、`models/`、`output/`。这些目录被 Git 忽略。首次下载需要互联网，实际推理不需要互联网。

本地 LLM 翻译另外执行：

```bash
bash scripts/install_llm_runtime.sh
python3 scripts/download_llm_model.py
cmake --build build -j2 --target translation_server
bash scripts/deploy_translation_service.sh
python3 scripts/evaluate_translation.py
python3 scripts/voice_translate.py \
  --wav models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/test_wavs/zh.wav \
  --source zh --target en
```

翻译网关编译需要 `libjsoncpp-dev`；`voice_translate.py` 会把译文交给 TTS 并保存目标语言 WAV。

识别公开中文样本：

```bash
./build/speech_cli stt \
  --model-dir models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17 \
  --vad-model models/silero_vad.onnx \
  --wav models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/test_wavs/zh.wav \
  --threads 4 --warmup 1 --repeat 5
```

合成中文和英文：

```bash
mkdir -p output
./build/speech_cli tts --backend piper-zh \
  --model-dir models/vits-piper-zh_CN-chaowen-medium \
  --text '你好，欢迎使用语音服务。' --output output/piper_zh.wav \
  --threads 4 --warmup 1 --repeat 5
./build/speech_cli tts --backend piper-en \
  --model-dir models/vits-piper-en_US-lessac-medium \
  --text 'Welcome to the offline speech system.' --output output/piper_en.wav \
  --threads 4 --warmup 1 --repeat 5
```

把 `--backend` 改为 `kokoro`、目录改为 `models/kokoro-int8-multi-lang-v1_1` 可运行多语言音色；中文示例用 `--speaker 3`，英文用 `--speaker 0`。`speech_cli` 返回单行 JSON，出错时返回非零退出码。输入目前为 WAV 文件，输出为 WAV 文件。

## 评测与项目资料

```bash
python3 scripts/evaluate.py --threads 2
python3 scripts/evaluate.py --threads 2 --vad --stt-only
python3 scripts/thread_sweep.py
python3 scripts/tts_compare.py
python3 scripts/soak_service.py --cycles 300
bash scripts/download_fleurs_dev.sh
python3 scripts/fleurs_eval.py --languages zh,en --limit 100 --strategies segments,gate,off
```

报告分别在 `output/evaluation/`、`output/thread_sweep/` 和 `output/soak/`。测试语音采用 SenseVoice 官方包中的中文和英文 WAV，以及固定版本的 [FLEURS](https://huggingface.co/datasets/google/fleurs) 中英文开发集；加噪样本用固定随机种子生成 20/10 dB SNR；再测 1 秒纯静音和合成语音回读。参见 [部署流程与优化](docs/deployment.md)、[本机实测结果](docs/results.md)、[翻译选型](docs/translation_choice.md) 和 [翻译实测](docs/translation_results.md)。

FLEURS 评测每种语言取 100 条；中文有效 100 条，英文因 30 秒输入上限有效 99 条。数据适合比较当前配置，但不能代表实际麦克风、方言和专业术语。TTS 的 STT 回读只检查可懂度的一个侧面，不能代替听测或语音质量评价。不同硬件、负载、文本长度与音色下的速度需要重测。

## 上游与许可

运行时及模型来自 [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) 的发布包；模型原始资料包括 [SenseVoice](https://huggingface.co/FunAudioLLM/SenseVoiceSmall)、[Kokoro](https://huggingface.co/hexgrad/Kokoro-82M-v1.1-zh) 和 [Piper 中文](https://k2-fsa.github.io/sherpa/onnx/tts/all/Chinese/vits-piper-zh_CN-chaowen-medium.html)、[英文](https://k2-fsa.github.io/sherpa/onnx/tts/all/English/vits-piper-en_US-lessac-medium.html)。本仓库不提交模型权重。若用于商业产品，应分别核对运行时、每个模型与音色的许可证及分发条款。
