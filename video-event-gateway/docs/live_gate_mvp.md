# OBS 直播传输闸门 MVP：实现与实测

> 2026-11-15。此版本只验证私有输入、有界缓冲、模拟拦截和放行输出。`--demo-allow` 不是内容审核，不应对真实有风险内容开放观众播放。

## 已实现的链路

```text
OBS / FFmpeg ── RTMP :11935 ── veg_live_gate (C++17 / FFmpeg)
                                   │ 只接收推流；不提供原始流的播放接口
                                   │ 有界内存缓冲；按视频关键帧组成 GOP
                                   │ 延迟放行；--demo-block START:END 模拟拦截
                                   ↓
                       独立 ZLMediaKit :11936 /approved/obs
                                   ├── RTSP :18555 → Ubuntu Qt 播放器
                                   └── HLS :18091 → 手机 / 浏览器
```

原始流由 `veg_live_gate` 的 FFmpeg RTMP 监听器直接接收，不发布到 ZLMediaKit。被模拟拦截的完整 GOP 不写到公开输出；时间戳压缩以连续播放。这一版本**剪掉**被拦截片段，未实现规划书中的占位画面/静音片段。启用真正 AI 前，需把判定接口接进缓冲阶段，确保超时和失败时默认拦截。

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

当前服务使用 `--delay-ms 8000`。计时从一个 GOP 接收完整后开始，所以观众实际看到的延迟还包含 GOP 时长和播放器缓冲；这是测试配置，不是已测得的端到端延迟。输入只监听板子的有线 IP，当前没有推流鉴权，仅限受控局域网测试。

## 板端构建与服务

板端已有 FFmpeg 8 的开发头文件和库，使用这套 SDK 编译；不安装系统包，也不下载模型：

```bash
cmake -S video-event-gateway/live-gate -B /home/orangepi/edge-av/live-gate/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DFFMPEG_DEV_ROOT=/opt/orangepi/kodi-ffmpeg
cmake --build /home/orangepi/edge-av/live-gate/build --target veg_live_gate -j2
```

板端当前实际部署在 `/home/orangepi/edge-av/live-gate/bin/veg_live_gate`，由上述独立 CMake 目标编译后安装。用户服务文件是 [`deploy/board/edge-av-live-gate.service`](../deploy/board/edge-av-live-gate.service)：OBS 停止后程序正常退出，systemd 自动重启以等待下一次推流。HLS 实例由 [`deploy/board/edge-av-hls.service`](../deploy/board/edge-av-hls.service) 单独运行，原 RTSP/RTMP 服务未被替换。服务标准输出与错误输出丢弃，避免在板端持久写日志；测试记录写到 Windows `E:\orange Pi 5 Pro\log`。

## 已完成的合成流验证

在板端内存盘生成 30 秒、10fps 的红/蓝/绿三段 H.264 画面，并加 AAC 测试音；模拟拦截 10–20 秒蓝色片段，延迟 3 秒后推到现有 ZLMediaKit。Ubuntu 虚拟机通过 RTSP 解码并在其内存盘临时采集 120 帧：红色 20 帧、绿色 100 帧、蓝色 **0 帧**，颜色顺序为红→绿。网关报告 `allowed_gops=9`、`blocked_gops=6`，发布器与网关退出码均为 0。测试临时素材及解码帧在验证后清理。

第二次合成测试给红/蓝/绿片段分别配 300/900/500 Hz 音频，模拟拦截中间 10 秒。虚拟机采到约 11.84 秒 PCM，按 0.2 秒窗测得 300 Hz 9 窗、500 Hz 50 窗、900 Hz **0 窗**。这证明模拟拦截的蓝色画面和对应音频未出现在**这两次客户端采样**中；还需覆盖不同 GOP 长度、断流/重连、队列溢出、时间戳异常及多客户端。

HLS 已通过独立 ZLMediaKit 实例验证：从虚拟机请求测试流 `hls.m3u8` 返回 HTTP 200 和 3 个 TS 分片；测试时 9 个媒体文件、约 840 KiB，均位于板端 `/run/user/1000/edge-av-hls/www` 内存盘，测试停止后自动清空。当前服务配置还未启用推流鉴权，**仅限受控局域网测试**；手机浏览器实机播放尚未验收。板子此前无散热，不做长时间满载测试。

OBS 实际测试中，推流由 Windows `192.168.1.100` 接入；板端输出经虚拟机解码了 90 帧 H.264 画面和 AAC 音频，编码信息为 1304×1080 视频、48 kHz 双声道音频。切换到独立 HLS 实例后，再用合成流通过完整闸门，虚拟机 Qt 播放器解码 30 帧；HLS 播放列表和首个 TS 分片均返回 HTTP 200，TS 同步字节为 `0x47`。这证明当前链路可用，但不是对真实直播内容审核准确率或持续运行的证明。
