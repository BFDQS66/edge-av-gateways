# Orange Pi 5 Pro (RK3588S, 16GB) 语音翻译部署

项目目录：`/home/orangepi/workspace/speech-translation-gateway`。通过 Windows 主机和虚拟机之间的 SSH 局域网传输复用现有模型；仅下载 ARM 版 sherpa-onnx 运行库，板端不下载模型。服务只监听板端环回地址，远程调试使用 SSH 端口转发。板端 systemd 单元设置 `StandardOutput=null` 和 `StandardError=null`；Windows 上的 `scripts/monitor_board.ps1` 每分钟把健康快照写入 `E:\orange Pi 5 Pro\log`。系统自身的 journal 管理记录由 Ubuntu 管理，脚本不会向板端写应用日志。

模型：SenseVoice Small INT8 (239MB)、Silero VAD (0.6MB)、Piper 中英文 (各约 63MB)、Kokoro INT8 多语种语音 (约 207MB)、Qwen3 1.7B Q4_K_M (1.28GB)。板端源码只构建 llama-server 和本项目的 C++ 服务，避免复制虚拟机的 x86 二进制。

```bash
cd ~/workspace/speech-translation-gateway
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSHERPA_ONNX_ROOT="$PWD/deps/sherpa-onnx-v1.13.8-linux-aarch64-shared-cpu-lib"
cmake --build build --parallel 4
bash scripts/deploy_board.sh
```

从 Windows 测试时，在一个终端运行：

```powershell
ssh -F none -N -L 8767:127.0.0.1:8767 -L 8765:127.0.0.1:8765 orangepi@192.168.1.101
```

然后访问 `http://127.0.0.1:8767/readyz`。`POST /v1/translate` 接受中英文本翻译；`POST /v1/speech-translate?source=zh&target=en` 接受 16kHz 单声道 PCM WAV，返回识别与翻译。旧 Piper 目标语音可通过 `POST http://127.0.0.1:8765/v1/tts/en` 合成；实时耳机播报改用独立的 Kokoro 服务 `127.0.0.1:8768/v1/tts/en` 或 `/zh`。

板端麦克风单次翻译，不写临时录音文件：

```bash
python3 scripts/board_mic_translate.py --source zh --target en --seconds 5
python3 scripts/board_mic_translate.py --source en --target zh --seconds 5 --play
```

板子自带麦克风，使用前运行 `bash scripts/board_audio_route.sh main` 选择板载 MIC；插入带麦克风的耳麦后用 `bash scripts/board_audio_route.sh headset` 选择耳麦输入。普通耳机只能播放。录音设备按 ES8388 的 ALSA ID 选择，避免声卡序号变化；驱动在 48kHz 双声道采集成功，再由已安装的 ffmpeg 在内存中转成 16kHz 单声道。板载 MIC 左声道实测电平明显高于右声道，客户端默认用左声道；可用 `--channel right|mix` 切换。翻译后总会调用目标语言 TTS，播放默认关闭，因为板子是否接有耳机或扬声器未知。Windows 端的监控脚本可用 `-Once` 单次运行，输出文件为 `speech-board-health-YYYY-MM-DD.jsonl`。

## 2026-10-17 板端验收

- 系统为 Ubuntu 26.04 aarch64、Linux 6.18.49-rockchip-rk3588；`orangepi` 用户的 systemd linger 已启用。三个服务和两个健康定时器均已启用。服务标准输出/错误均设置为 `null`，主机监控每分钟在 E 盘记录 JSONL；Windows 主机关闭时无法继续生成 E 盘日志。
- 模型、VAD 和两套 TTS 的板端 SHA-256 与虚拟机文件一致；Qwen GGUF 的 SHA-256 为 `d2387ca2dbfee2ffabce7120d3770dadca0b293052bc2f0e138fdc940d9bc7b5`。ARM sherpa-onnx 官方归档 SHA-256 为 `fb98b80628a383909bf83626daca2e0d0ffc93bf1f7d222eaaa4282b388f072f`。
- 部署目录约 1.9GB；根分区 57GB、部署后可用约 48GB。没有把虚拟机的 x86 库、Kokoro 和 FireRed 对照模型复制到板上。Qwen 进程在 20 次请求后 RSS 约 2.37GiB，语音进程约 577MiB，网关约 8MiB；交换区未使用。
- 中文官方样本 STT 输出“开饭时间早上9点至下午5点。”，板端请求耗时约 2.15 秒（首次）；英文 TTS 返回 68KB WAV。一次完整语音翻译：STT 408ms、LLM 2441ms、网关 HTTP 总计 2853ms，随后英文 TTS 802ms，输出 191KB RIFF WAV。文本翻译 8 条样本平均 1712ms；4 条数字样本均保留了字面数字。20 次顺序翻译均成功，HTTP 平均 1134ms、P95 1622ms；4 并发突发为 1 次 200、3 次受控 503。
- 板载 MIC 的 `hw:rockchipes8388,0` 在 48kHz 双声道采集成功；安静环境 3 秒采样 RMS 约 36/32768。内存中重采样、VAD 和网关调用成功，因无人讲话返回空文本。随后真人 10 秒录音 RMS 约 364/32768，STT 识别“闭像头，请在下午5点前关闭摄像头。”，翻译成 “Close the camera before 5 o'clock PM.”。首词“闭像头”是多余识别，需进一步调输入起点、VAD/切段和麦克风位置。用该译文合成的英文 RIFF WAV 已经保存在 Windows `E:\orange Pi 5 Pro\output\speech_translation_live_en.wav`，没有在板端保存录音或结果音频。
- 编译 llama.cpp 时 4 个并行编译任务曾把 CPU 传感器推至约 92°C，最终构建成功；短时翻译负载后传感器约 70°C。后续音视频同机长期运行要监测散热和节流，尚未完成 24 小时稳定性测试。
- 故意向 `llama-translate.service` 与 `translation-gateway.service` 发送 SIGKILL，systemd 自动恢复 `/readyz` 分别耗时约 8.34 秒和 4.15 秒。该测试会让两个服务的重启计数各增加 1。

测试输出在 Windows `E:\orange Pi 5 Pro\log` 的 `speech-board-validation-2026-10-17.jsonl`、`speech-board-evaluation-2026-10-17.txt`、`speech-board-soak-2026-10-17.json`、`speech-board-recovery-2026-10-17.json`、`speech-board-live-mic-retry-2026-10-17.jsonl` 和每日日志中。测试揭示模型质量限制：中文官方样本把“开放时间”识别成“开饭时间”，LLM 依错误文本译出“Breakfast time”；另一次“你好，今天下午五点开饭。”被译成“Good afternoon, 5 o'clock PM.”，漏掉“开饭”。目前不能把 8 条冒烟样本当作翻译准确率，下一步需要更多真人语音、领域语料与人工参考译文来做质量评估。

## 持续监听与屏幕界面

`bash scripts/deploy_board.sh` 现在还会安装 `live-translate.service`、`live-translate-display.service` 与每分钟健康定时器。板子登录 GNOME 后，Chromium 以应用窗口打开 `http://127.0.0.1:8770/`；可在板子浏览器手动输入该地址。页面可选“中文 → English”或“English → 中文”，显示原文、译文、各阶段耗时、麦克风电平与历史结果；还可关闭耳机播报或点“试听耳机”。切换方向从下一句开始生效，设置持久保存在板端 `config/live.json`（几十字节，不是日志）。即使关闭浏览器，`live-translate.service` 仍持续监听。界面只绑定 `127.0.0.1`，其他电脑需通过 SSH 转发 `8770` 端口访问。

采集链为 ES8388 左声道 48kHz 双声道 PCM，经 ffmpeg 管道转为 16kHz 单声道。100ms 一帧，动态噪声底线与最小 RMS 100～220 门限检测讲话。电平较强的短句在约 1 秒静音后提交，连续讲话最长约 8 秒强制提交并保留末尾 300ms；板载小孔麦克风实测电平较弱，这类输入保留约 12 秒上下文再识别，以减少 1～2 秒短片段被 VAD 丢弃的问题。若长片段的门控识别为空，且电平明显高于底噪，才以无 VAD 模式补试一次，并按源语言过滤过短的结果。采集线程始终继续收音，单独的推理线程调用 STT 与 LLM。独立 TTS 线程调用 Kokoro C++ 服务，播放线程把 24kHz WAV 在内存中响度标准化、用 SoXR 高质量重采样为 48kHz 双声道 PCM，再用 `aplay -D hw:rockchipes8388,0` 直送耳机。语音数据与 TTS WAV 都只在内存中，不在板端落盘。推理、TTS 和播放队列各最多等 3 句，满时计数并记录丢弃事件；翻译网关偶发 HTTP 503 时会短暂重试。结果缓存在内存中的最近 1000 个事件，服务重启后清空。

Windows 已可运行 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/collect_live_board.ps1` 持续收集结果和错误；`-Once` 只轮询一次。结果写到 `E:\orange Pi 5 Pro\log\speech-live-events-YYYY-MM-DD.jsonl`，去重游标写到同目录的 `speech-live-cursor.json`。原有 `monitor_board.ps1` 每分钟采集服务、麦克风、温度、磁盘和错误计数。主机离线时 E 盘无法收集；板端仅保留内存中的最近事件，因此离线过久可能丢失较旧结果。服务 stdout/stderr 设为 `null`，脚本不把应用日志写到板卡。

Windows 已注册 `OrangePiSpeechLiveCollector` 和 `OrangePiSpeechHealthMonitor` 两个当前用户登录触发的计划任务；此会话中也已经以隐藏进程启动。计划任务仅在 Windows 已登录且能连接板子时工作。重新安装计划任务可运行 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/install_windows_monitors.ps1`。两个脚本使用互斥锁防止重复实例。收集脚本显式以 UTF-8 解析 SSH 输出，保留中文原文。

```bash
systemctl --user status live-translate.service live-translate-display.service
curl -fsS http://127.0.0.1:8770/api/metrics
curl -fsS http://127.0.0.1:8770/healthz
```

真人语音验收前，可看 `/api/metrics` 中 `rms`、`threshold`、`noise_rms`，以及 `segments` 与 `recognized` 是否增加。语音识别和翻译计时分别是 `last_stt_ms` 与 `last_translation_ms`；`last_display_delay_ms` 是音频切段后到译文生成，浏览器 1 秒轮询会再增加最多约 1 秒。较强、短句的历史实测：STT 243/288ms、翻译 457/770ms、切段后译文生成 747/1063ms。当前板载小孔麦克风的低电平输入要等约 12 秒片段收齐，首个结果延迟因而明显更长；回放测试不能代表实时响应上限。该界面按音频片段显示结果，尚不提供字级实时字幕。长期运行所需的自动恢复与有界队列已配置，仍需实际连续运行来验证故障率和识别质量。

耳机插入时 ES8388 的 `Headphones Jack` 状态为 `on`。2026-10-17 的 Piper 冒烟测试虽然播放成功，但真人试听反馈音调异常、发音含糊。2026-10-18 改用 Kokoro 后，同一句英文真人试听反馈“明显更好，音调正常，仍有一点粗糙”。Kokoro 板端生成 2.26 秒英文约需 6 秒、3.31 秒中文约需 7.2 秒；独立 TTS 服务与识别线程分开，以免耳机播报拖住新一段原文和译文的显示。实时页面的耳机试听实测 Kokoro 合成 3.64 秒、响度处理及重采样 154ms、播放 977ms；服务无错误。英文官方 WAV 通过 `POST /v1/speech-translate?source=en&target=zh` 也可识别英文并返回中文译文。注意源语言由页面显式选择，当前网关不判断讲话语种；选英译中时说中文可能得到无意义的中文改写。

2026-10-18 免屏幕真人长句首轮测试切出 5 段，每段约 1.6～2.4 秒，仅 1 段得到识别文本。随后短暂暂停实时服务，把约 60 秒小孔麦克风输入录入 `/dev/shm` 内存盘，录完立即恢复服务。左右声道都可识别；录音的最高 100ms 帧 RMS 为 293，95 分位为 180，安静底噪约 45。用当前 12 秒弱语音切段算法离线回放，4 段中 3 段门控识别成功，另 1 段经受控无 VAD 补试识别并成功译成英文；原先的短切段算法回放 14 段只识别 5 段。回放的文本仍有错字和漏词，因此只是改进，不是准确率达标证明。纯静音绕过 VAD 曾幻觉出韩语“그.”，补试条件和短结果过滤必须保留。`/api/metrics` 可查看 `forced_chunks`、`weak_fallback_chunks`、`stt_fallback_attempts`、`stt_fallback_accepted`、`queue_depth`、`tts_queue_depth`、`playback_errors`；Windows E 盘 `speech-live-events-YYYY-MM-DD.jsonl` 记录切段、翻译与播放事件。下一步在真实连续输入中核对识别质量、队列积压和端到端延迟。

新版服务的随后一次连续播放实测：14 段音频中 10 段有中文原文和英文译文，4 段门控识别为空；其中 1 段由无 VAD 补试恢复。10 段译文全部完成 Kokoro 合成和耳机播放，推理、合成和播放队列最终排空，服务错误、采集重启及队列丢弃均为 0。切段后译文生成耗时的中位数约 12.4 秒、最大约 16.4 秒；低电平输入还需先累积约 12 秒，所以端到端首段字幕可达二十多秒。合成单段最长约 22.8 秒，持续负载时 TTS 队列最高积压 3 段。这次没有丢播报，但更长连续输入可能使队列满而丢弃播报；完整文本仍先显示。板子没有任何散热件，Windows 健康监控记录 CPU 温度最高约 89.4°C，停止播放后约 78～79°C。装散热片和风扇后需再测温度、延迟和长时队列；目前不能把这次几分钟测试当作长期稳定性证明。回放明细与实时统计分别保存在 E 盘 `mic-route-replay-2026-10-18.json` 和 `live-retest-2026-10-18.json`，内存盘录音已删除。

同日切到 English → 中文后，板载麦克风把一次用户英文尝试分成 “Please close the camera.” 和 “Before5 clock.”；对应译文为“请关闭摄像头。”与“前5钟。”，均完成中文耳机播报。用户说明英语发音不熟练，因此这次真人尝试不能用来衡量模型的英语识别率。标准英文 WAV 经网关识别出完整英文句，STT 约 0.50 秒、中文翻译约 1.85～3.56 秒。测试还发现一次环境声被识别成单独句号，LLM 随后生成无关内容；现已在实时服务和 C++ 翻译网关拦截无字母、数字或汉字的结果。`check_translation_api.py` 验证标点文本返回 HTTP 400、服务就绪；标准英文 WAV 在新版网关仍能正确识别翻译。随后触发固定中文耳机试听“摄像头已经关闭，请把门关上。”，Kokoro 合成约 7.5 秒、播放约 3.6 秒、零错误；用户确认中文发音清楚、音调正常。

再次播放约 40～50 秒英文新闻音频的实时测试中，5 次切段有 4 段产出中文译文、1 段识别为空，2 段使用低电平无 VAD 补试；4 段译文全部完成耳机播报，服务错误和队列丢弃为 0。第一段音频长 8 秒，切段后约 3.2 秒显示译文；4 段的切段后显示耗时中位数约 4.6 秒，最大约 8.4 秒。当前按音频片段输出，不是字词级同步字幕；低电平语音通常要先积累约 12 秒。健康监控在连续负载中记录 CPU 约 84.1°C，板子仍无散热。E 盘 `long-english-retest-2026-10-18.json` 保存了每段原文、译文和耗时。

长句中一次 Qwen3-1.7B 译文大量复制英文。复测发现将该句按逗号拆成两段后，两段都可以译成中文。C++ 网关现在仅对目标为中文、长原文且输出英文比例过高的结果作最多 3 段的分句重试；正常路径不变。`check_translation_repair.py` 在板端验证此句修复后已无英文长句，约 6.5 秒完成。译文中的地名仍有误译和语义重复，分句策略改善语言完整性，但不能保证语义准确；后续应使用更合适的翻译模型或加上下文评测。
