# OBS 直播传输闸门 MVP：实现与实测

> 2026-11-15。已验证私有输入、有界缓冲、发布鉴权和实验性 STT 文本规则。规则目前仅使用“测试拦截/测试阻断”两个演示短语；没有完成真实违规类别的准确率评测，不应对真实有风险内容开放观众播放。

## 已实现的链路

```text
OBS / FFmpeg ── RTMP :11935 ── veg_live_gate (C++17 / FFmpeg)
                                   │ 只接收推流；不提供原始流的播放接口
                                   │ 有界内存缓冲；按视频关键帧组成 GOP
                                   │ 延迟放行；allow / review / block 三态
                                   │ AAC 解码 → 板端 SenseVoice STT → 演示文本规则
                                   ↓
                       独立 ZLMediaKit :11936 /approved/obs
                       on_publish：板端来源 + 仅限本流 + 临时令牌
                                   ├── RTSP :18555 → Ubuntu Qt 播放器
                                   └── HLS :18091 → 手机 / 浏览器
```

原始流由 `veg_live_gate` 的 FFmpeg RTMP 监听器直接接收，不发布到 ZLMediaKit。闸门可使用 `--demo-block` / `--demo-review` 模拟判定，也可把 GOP 内 AAC 解码、在内存中转成 16 kHz 单声道 WAV，调用已有的板端 STT 服务。`block` 与 `review` 的完整 GOP 都**不发布**；失败或超时归为 `review`。被拦截的片段目前被**剪掉**，没有占位画面、静音片段、人工复核队列。发布鉴权只保护输出推流入口，OBS 输入仍只限受控局域网测试。

实现位置：[`src/live_gate.cpp`](../src/live_gate.cpp)。FFmpeg 开发文件可由 `FFMPEG_DEV_ROOT` 指向板子已存在的 `/opt/orangepi/kodi-ffmpeg`，无需额外下载。源代码与旧视频事件程序一起由 CMake 管理；只在找到 FFmpeg 开发文件时构建 `veg_live_gate`。

## OBS 测试参数

在 OBS 的 **设置 → 直播** 选择“自定义”：

| 参数 | 值 |
| --- | --- |
| 服务器 | `rtmp://192.168.1.101:11935/live` |
| 串流密钥 | `obs` |
| 输出 | H.264 视频、AAC 音频；先用 720p/30fps 或更低测试 |

此阶段只推送自制测试画面或获授权的素材。板端 `edge-av-live-gate.service` 接收后发布为 `rtsp://192.168.1.101:18555/approved/obs`，HLS 地址为 `http://192.168.1.101:18091/approved/obs/hls.m3u8`；在 Ubuntu 虚拟机可用 GStreamer 验证：

```bash
gst-launch-1.0 -v rtspsrc \
  location=rtsp://192.168.1.101:18555/approved/obs protocols=tcp latency=200 \
  name=src src. ! 'application/x-rtp,media=video' ! \
  rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! autovideosink
```

当前服务使用 `--delay-ms 8000`。计时从一个 GOP 接收完整后开始，所以观众实际看到的延迟还包含 GOP 时长、STT 耗时和播放器缓冲；这是测试配置，不是已测得的端到端延迟。输入只监听板子的有线 IP，当前没有 OBS 推流鉴权，仅限受控局域网测试。

## 板端构建与服务

板端已有 FFmpeg 8 的开发头文件和库，使用这套 SDK 编译；不安装系统包，也不下载模型：

```bash
cmake -S video-event-gateway/live-gate -B /home/orangepi/edge-av/live-gate/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DFFMPEG_DEV_ROOT=/opt/orangepi/kodi-ffmpeg
cmake --build /home/orangepi/edge-av/live-gate/build --target veg_live_gate -j2
```

板端当前实际部署在 `/home/orangepi/edge-av/live-gate/bin/veg_live_gate`，由上述独立 CMake 目标编译后安装。用户服务文件是 [`deploy/board/edge-av-live-gate.service`](../deploy/board/edge-av-live-gate.service)：OBS 停止后程序正常退出，systemd 自动重启以等待下一次推流。HLS 实例由 [`deploy/board/edge-av-hls.service`](../deploy/board/edge-av-hls.service) 单独运行。原测试服务 `edge-av-zlm.service` 的程序和配置保留供回退，但已停用并禁用自启动，旧的 `1935/8554/18089` 端口不再开放。服务标准输出与错误输出丢弃，避免在板端持久写日志；测试记录写到 Windows `E:\orange Pi 5 Pro\log`。

放行服务用 [`publish_auth.py`](../scripts/publish_auth.py) 实现 ZLMediaKit `on_publish` hook。它只接受板端来源 IP、`approved/obs`、RTMP 和与板端 `/home/orangepi/edge-av/run/publish-token` 相符的随机令牌。令牌文件权限是 `0600`，不入库、不输出到日志；闸门从文件读取并追加到内部发布 URL。首次部署需在板端生成令牌文件，并通过 [`configure_publish_auth.py`](../scripts/configure_publish_auth.py) 原子修改私有的 `hls-config.ini`，然后启用 [`edge-av-publish-auth.service`](../deploy/board/edge-av-publish-auth.service)、重启 HLS 和闸门。服务文件中的板端 IP 需与实际有线 IP 一致。Hook 停止时，依赖它的 HLS 服务也会停止，放行端口拒绝连接。该设计的信任边界是受控板端账号；拥有 `orangepi` 账号及令牌文件读取权的本机进程仍可伪装发布者。

板端常驻服务当前选择 `--asr-port 8765`，对识别文本精确匹配演示短语：“测试拦截”进入 `review`，“测试阻断”进入 `block`；其他内容仅代表**未命中演示规则**。STT 每个约 2 秒 GOP 单独运行，跨 GOP 语句可能漏识别；真正审核需要更长上下文、视觉模型、分类评测和人工复核流程。板端已有语音服务提供 SenseVoice STT，闸门不会下载第二份模型。此处 Python 只负责部署配置和发布鉴权，音频解码、缓冲、判定和发布是 C++。

## 已完成的合成流验证

在板端内存盘生成 30 秒、10fps 的红/蓝/绿三段 H.264 画面，并加 AAC 测试音；模拟拦截 10–20 秒蓝色片段，延迟 3 秒后推到现有 ZLMediaKit。Ubuntu 虚拟机通过 RTSP 解码并在其内存盘临时采集 120 帧：红色 20 帧、绿色 100 帧、蓝色 **0 帧**，颜色顺序为红→绿。网关报告 `allowed_gops=9`、`blocked_gops=6`，发布器与网关退出码均为 0。测试临时素材及解码帧在验证后清理。

第二次合成测试给红/蓝/绿片段分别配 300/900/500 Hz 音频，模拟拦截中间 10 秒。虚拟机采到约 11.84 秒 PCM，按 0.2 秒窗测得 300 Hz 9 窗、500 Hz 50 窗、900 Hz **0 窗**。这证明模拟拦截的蓝色画面和对应音频未出现在**这两次客户端采样**中；还需覆盖不同 GOP 长度、断流/重连、队列溢出、时间戳异常及多客户端。

HLS 已通过独立 ZLMediaKit 实例验证：从虚拟机请求测试流 `hls.m3u8` 返回 HTTP 200 和 3 个 TS 分片；测试时 9 个媒体文件、约 840 KiB，均位于板端 `/run/user/1000/edge-av-hls/www` 内存盘，测试停止后自动清空。现在已启用放行服务器的推流鉴权；手机浏览器实机播放尚未验收。板子此前无散热，不做长时间满载测试。

OBS 实际测试中，推流由 Windows `192.168.1.100` 接入；板端输出经虚拟机解码了 90 帧 H.264 画面和 AAC 音频，编码信息为 1304×1080 视频、48 kHz 双声道音频。切换到独立 HLS 实例后，再用合成流通过完整闸门，虚拟机 Qt 播放器解码 30 帧；HLS 播放列表和首个 TS 分片均返回 HTTP 200，TS 同步字节为 `0x47`。这证明当前链路可用，但不是对真实直播内容审核准确率或持续运行的证明。

## 发布鉴权、STT 和短时稳定性验收（2026-11-15）

- Ubuntu 虚拟机直接向 `:11936/approved/obs` 推流失败；板端进程不带令牌直推同样返回 `Operation not permitted`。合成流经带令牌的 C++ 闸门发布后，HLS 播放列表返回 HTTP 200。令牌值未写入仓库或测试记录。
- 板端已有 TTS 合成“现在进行测试拦截”，STT 单独识别为“现在进行测试拦截。现在进行测试拦截。”，一次请求耗时约 358 ms。合成音视频经闸门时，`--review-term 测试拦截` 使前三个 GOP 进入 `review`；换成 `--block-term 测试拦截`，前三个 GOP 进入 `block`，其余一个 GOP 放行。`asr_calls=4`、`asr_errors=0`。这只证明演示词的单次端到端判定，不是审核准确率。
- STT 指向不可用的本机端口时，3 个 GOP 全部进入 `review`，`allowed_gops=0`，没有连接公开输出。模型异常不会触发默认放行。
- 1 路 160×120、5 fps、AAC 合成流连续推送 60 秒，HLS 从第 15 秒起返回 HTTP 200，源推流正常退出。每 5 秒采集的板端最高温度为 71.153°C，闸门 RSS 峰值约 21.2 MiB，未触及 80°C 提前终止阈值；采样记录在 Windows `E:\orange Pi 5 Pro\log\edge-av-short-soak-2026-11-15.txt`。这是短时低分辨率检查，不能替代装散热后的 24/72 小时测试。
