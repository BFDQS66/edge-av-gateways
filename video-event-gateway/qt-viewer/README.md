# Qt 6 单路直播播放器

在 Ubuntu 虚拟机上用 C++17 / Qt 6 Widgets + GStreamer 播放板端 `approved` RTSP 流。GStreamer 负责 RTSP/H.264/AAC 解码；Qt 显示帧和播放状态。当前一个 `StreamTile` 管理一套播放管线、画面和重连状态，主窗口用网格容纳它；以后可以加入多个 Tile。该界面仅是观看端，**不会执行内容审核**。

## 构建

Ubuntu 24.04 依赖 `qt6-base-dev`、`libgstreamer1.0-dev`、`libgstreamer-plugins-base1.0-dev` 和 H.264/AAC 解码插件。虚拟机中已有 GStreamer 开发包，现已安装 Qt 6 开发包。全新环境可执行：

```bash
sudo apt install --no-install-recommends qt6-base-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-libav gstreamer1.0-plugins-good
cmake -S qt-viewer -B build/qt-viewer -DCMAKE_BUILD_TYPE=Release
cmake --build build/qt-viewer -j2
```

在虚拟机上执行 `bash qt-viewer/install_vm.sh` 可编译到项目的 `qt-viewer/build`，安装程序到 `~/.local/bin`，并在应用菜单加入“Edge AV 直播播放器”。脚本不安装系统软件包，也不设置开机自启。

默认打开 `rtsp://192.168.1.101:18555/approved/obs`。启动桌面界面：

```bash
./build/qt-viewer/veg_qt_viewer
```

可在地址栏更改 RTSP 地址，按“连接”切换流，“停止”主动停止；有音量和静音控制。网络/媒体错误后每 3 秒尝试重连，20 秒没有视频帧也会重连。OBS 未推流时画面为空并等待重连。

无桌面环境的自动解码检查（30 帧）：

```bash
QT_QPA_PLATFORM=offscreen ./build/qt-viewer/veg_qt_viewer \
  --probe --url rtsp://192.168.1.101:18555/approved/obs --timeout 35
```

初版使用 Qt Multimedia 的 `QVideoWidget` 时，虚拟机桌面截图只显示黑屏；切换其 FFmpeg 后端在 VMware 的 Vulkan 解码路径发生 FPE。因此当前实现让已验证的 GStreamer 解码并把 RGB 帧送给 Qt `QLabel`，音频由 GStreamer 输出。后续多路时需要测解码 CPU、内存与实际显示帧率。

2026-11-15 在 Ubuntu 24.04 虚拟机中完成编译，`--probe` 从审核网关的 RTSP 输出解码 30 帧并退出码为 0；实际桌面窗口截图可见 OBS 画面，状态为“正在播放”。切换到独立 HLS 实例的 RTSP 端口后，合成流又通过了 30 帧探针；断流后第二次推入合成流，原窗口无需手动操作即重新显示画面。VM 上的部署目录是 `/home/fu-shanfeng/edge-av/qt-viewer`，现有窗口由临时用户服务 `edge-av-qt-viewer-hls.service` 启动。用户尚未反馈主观音质；多路和长期稳定性未测试。
