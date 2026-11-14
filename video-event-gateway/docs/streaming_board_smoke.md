# Orange Pi 5 Pro：RTSP/RTMP 合成流基线

2026-11-14 在 RK3588S、16 GiB 的 Orange Pi 5 Pro 上完成双协议回环。CSI 摄像头暂未枚举，故以 FFmpeg 合成 H.264 画面测试；这证明媒体服务器的发布和读取链路，不代表摄像头或 AI 已部署。

## 组件和目录

- 媒体服务器：[ZLMediaKit](https://github.com/ZLMediaKit/ZLMediaKit) 固定提交 [`fb3265725f0880da1f52a3cc6e6eb04bbc327fa3`](https://github.com/ZLMediaKit/ZLMediaKit/commit/fb3265725f0880da1f52a3cc6e6eb04bbc327fa3)。源码和子模块先下载到 Windows，再经局域网传到板端，未下载到板端或纳入本仓库。
- 板端源码及构建：`/home/orangepi/edge-av/third_party/ZLMediaKit`，当前约 118 MiB；传输用的 8 MiB 压缩包已删除。
- 私有运行配置：`/home/orangepi/edge-av/run/config.ini`，权限 `0600`，API 密钥由脚本随机生成，不在 Git 中。
- 用户服务：[edge-av-zlm.service](../deploy/board/edge-av-zlm.service)，使用已有的 `Linger=yes` 保持登录外运行；日志只写 `/run/user/1000/edge-av-zlm/log` 内存盘，单片 2 MiB、最多两片，标准输出不进入 journal。
- Windows 构建/验收输出：`E:\orange Pi 5 Pro\log\streaming_setup_2026-11-14.log` 和 `streaming_smoke_2026-11-14.log`。

## 构建与启动

在 Windows 获取固定提交及其子模块，然后把源码通过 `scp` 传到板端独立目录。板端构建命令：

```bash
cmake -S /home/orangepi/edge-av/third_party/ZLMediaKit \
  -B /home/orangepi/edge-av/third_party/ZLMediaKit/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_TESTS=OFF -DENABLE_WEBRTC=OFF -DENABLE_SRT=OFF \
  -DENABLE_HLS=OFF -DENABLE_MP4=ON -DENABLE_API=OFF \
  -DDISABLE_REPORT=ON -DENABLE_OBJCOPY=OFF
cmake --build /home/orangepi/edge-av/third_party/ZLMediaKit/build \
  --target MediaServer -j2
```

当前上游提交在关闭 `ENABLE_MP4` 时编译 `AV1.cpp` 会找不到子模块中的 `aom-av1.h`；故保持 `ENABLE_MP4=ON` 以完成构建。运行配置中的 `protocol.enable_mp4=0`，不会主动录像。`ENABLE_API=OFF` 关闭的是 SDK C API 库，HTTP 管理接口仍可用。

把本仓库的 `scripts/render_zlm_config.py` 和 `scripts/smoke_zlm.sh` 复制到板端 `/home/orangepi/edge-av/scripts/`，生成配置：

```bash
python3 /home/orangepi/edge-av/scripts/render_zlm_config.py \
  --sample /home/orangepi/edge-av/third_party/ZLMediaKit/conf/config.ini \
  --output /home/orangepi/edge-av/run/config.ini \
  --listen-ip 192.168.1.101
```

渲染器只创建新文件，拒绝覆盖已有密钥。要更新配置，先输出到 `config.next.ini`，核对后再移动替换。它把未编译的 HLS、TS、fMP4 输出关闭，避免推流时被上游默认配置拒绝；还关闭额外的 RTP 代理、ONVIF、HTTPS/RTMPS/RTSPS 监听端口。

把本仓库的服务文件复制到 `~/.config/systemd/user/edge-av-zlm.service`，然后运行：

```bash
systemctl --user daemon-reload
systemctl --user enable --now edge-av-zlm.service
systemctl --user is-active edge-av-zlm.service
```

服务仅绑定板子的有线地址 `192.168.1.101`。端口是 RTSP `8554`、RTMP `1935`、HTTP API `18089`；更换有线地址时需重新生成运行配置。管理接口密钥不通过公开 README 或命令示例传递。

发布路径为 `rtmp://192.168.1.101:1935/live/synthetic`；有发布器在线时可从 `rtsp://192.168.1.101:8554/live/synthetic` 或同一 RTMP URL 读取。服务空闲时没有视频流。

## 不停机回归测试

可用备用端口启动临时实例，测试期间正式服务继续运行：

```bash
python3 /home/orangepi/edge-av/scripts/render_zlm_config.py \
  --sample /home/orangepi/edge-av/third_party/ZLMediaKit/conf/config.ini \
  --output /dev/shm/edge-av-smoke-config.ini \
  --listen-ip 192.168.1.101 \
  --http-port 18090 --rtsp-port 18554 --rtmp-port 11935
bash /home/orangepi/edge-av/scripts/smoke_zlm.sh \
  /home/orangepi/edge-av/third_party/ZLMediaKit/release/linux/Release/MediaServer \
  /dev/shm/edge-av-smoke-config.ini 192.168.1.101 18554 11935
rm -- /dev/shm/edge-av-smoke-config.ini
```

测试脚本生成 `640×360@15fps` H.264 画面，通过 RTMP 推流，再从 RTSP 和 RTMP 各解码 10 帧。服务器、发布器和临时日志在脚本退出时清理。临时配置位于内存盘，最后一条命令清除它。

跨设备测试时，在板端向常驻服务短时推送相同的合成流，然后在 Windows 执行：

```powershell
python video-event-gateway/tests/check_stream_clients.py --host 192.168.1.101 --frames 10
```

此脚本需要 Windows Python 的 OpenCV FFmpeg 后端，只用于客户端回归；板端运行服务不依赖它。测试输出保存在 Windows 的上述 E 盘日志中。

## 实测与边界

| 项目 | 结果 |
| --- | --- |
| RTSP 回环 | 解码 10 帧，脚本输出 `RTSP_OK` |
| RTMP 回环 | 解码 10 帧，脚本输出 `RTMP_OK` |
| Windows → 板端 | TCP `8554`、`1935`、`18089` 均可连接；Windows OpenCV 从 RTSP 和 RTMP 各解码 10 帧，分辨率均为 640×360 |
| 单次客户端打开耗时 | 本次 RTSP 约 1.39 秒、RTMP 约 5.05 秒；RTMP 过程中 OpenCV 输出一次 5 秒超时警告，尽管读帧成功，启动延迟仍需后续排查 |
| HTTP 管理接口 | `getMediaList` 返回 `code=0`；服务空闲时流数为 0 |
| 服务状态 | `edge-av-zlm.service` active，检查时重启次数 0 |
| 资源抽样 | 空闲 MediaServer RSS 约 25.8 MiB；编译期间观察到最高温约 76°C；构建后根分区仍有约 48 GiB 可用 |

尚未验收 CSI 出图、硬件编码、RKNN、24 小时稳定性或多客户端并发。上述打开耗时仅是单次合成流测量，不能视为延迟分布。合成发布器测试结束即停止，正式服务保持空闲运行，不持续占用板端编码 CPU。
