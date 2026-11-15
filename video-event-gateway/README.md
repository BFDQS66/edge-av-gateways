# Video Event Gateway

**当前新方向：[EdgeLiveGuard：OBS 直播音视频审核网关规划书](docs/live_moderation_plan.md)。** OBS 通过 RTMP 推入接收专用端口；C++ 网关有界缓存音视频，再发布到独立 ZLMediaKit 的 RTSP/HLS 输出。传输闸门原型已实现，真正的 AI 审核、推流鉴权和手机浏览器实机播放尚未实现；原有越线检测代码是早期视频事件原型。新方向不依赖 CSI 摄像头。

已新增[直播传输闸门 MVP](docs/live_gate_mvp.md)：C++/FFmpeg 接收 OBS RTMP，按 GOP 在有界内存中延迟发布，可用命令行模拟拦截时间段。**当前 `--demo-allow` 会放行其他所有时段，不含 AI 审核，不可把它当作内容安全服务。** OBS 实际推流、虚拟机[Qt 单路播放器](qt-viewer/README.md)和 HLS 播放列表/TS 分片已验证。

## 早期视频事件原型

Ubuntu 上运行的 C++17 多路视频事件网关。它从文件或 RTSP 拉取视频，用 OpenCV 处理图像，选择运动检测或 ncnn YOLOv8 人员检测，给目标分配轨迹 ID，再执行越线、区域进入和滞留规则。事件写入 JSONL，证据保存为 PNG；Drogon 提供状态、事件查询和 Prometheus 文本指标。

原有的事件网关开发和验证在 Ubuntu 虚拟机完成。Orange Pi 5 Pro 上已部署独立的 ZLMediaKit 媒体服务，并用合成视频完成 RTSP/RTMP 双协议回环和 Windows 跨设备解码；CSI 摄像头、板端 AI 和两者集成仍待完成。详见[板端流媒体基线](docs/streaming_board_smoke.md)。项目中附带的 `veg_rtsp_server` 继续用于离线测试录像。

早期基于 CSI 摄像头的构想保存在[旧实施规划](docs/streaming_roadmap.md)，供对照历史设计；当前实施顺序以直播审核规划书为准。

```text
视频文件 / 摄像头 RTSP
          │
          ├── 每路独立采集与重连
          └── 检测器（运动 / ncnn YOLOv8 人员）
                    ↓
               目标关联与轨迹
                    ↓
          越线 / 区域进入 / 滞留
                    ├── JSONL + PNG 证据
                    └── Drogon HTTP API + 指标
```

## 快速运行：两路 RTSP 测试

Ubuntu 24.04 依赖：C++ 编译器、CMake 3.16+、Python 3、OpenCV 4 的 core/imgproc/imgcodecs/videoio、Drogon、JsonCpp、GStreamer RTSP Server 和 H.264 编码插件。在本虚拟机中这些开发包已经安装。全新 Ubuntu 可以执行：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config python3 git \
  libopencv-core-dev libopencv-imgproc-dev libopencv-imgcodecs-dev \
  libopencv-videoio-dev libdrogon-dev libjsoncpp-dev \
  libgstrtspserver-1.0-dev gstreamer1.0-plugins-ugly gstreamer1.0-libav
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

从项目根目录开两个终端：

```bash
# 终端 1：生成录像并发布 RTSP
mkdir -p data
./build/veg_generate_fixture data/synthetic_crossing.avi
./build/veg_rtsp_server --file data/synthetic_crossing.avi --port 8554
```

```bash
# 终端 2：两路并发拉流，处理各 120 帧
./build/veg_gateway --config config/gateway.yaml --max-frames 120
```

另开终端查询：

```bash
curl -s http://127.0.0.1:8088/health
curl -s 'http://127.0.0.1:8088/api/v1/events?camera_id=camera_a'
curl -s http://127.0.0.1:8088/metrics
```

每路事件在 `output/gateway/<camera_id>/events.jsonl`，同目录有 PNG 截图。`--max-frames` 用于回归测试；省略后网关持续运行。将 `config/gateway.yaml` 的 `sources[].uri` 改为真实 RTSP URL 即可接入摄像头。`veg_rtsp_server --test-pattern --port 8554` 可以发布连续测试画面。文件模式播放到末尾后，该次媒体结束，服务器继续等待客户端。

## 真实人物检测：ncnn 可选后端

网关默认使用运动检测，无需模型。要启用 CPU 版 ncnn，先在项目之外构建 [Tencent/ncnn](https://github.com/Tencent/ncnn)，再取 [Qengineering 的 YOLOv8 ncnn 示例](https://github.com/Qengineering/YoloV8-ncnn-Raspberry-Pi-4)中的演示模型与图片。脚本将示例固定在提交 `3342965252320bb4aca1835efbd115bef4653034`，下载内容放在被 Git 忽略的 `data/` 和 `.cache/` 目录。第三方推理适配代码的许可证保存在 `third_party/yolov8_ncnn/LICENSE`。

```bash
mkdir -p ../deps
git clone https://github.com/Tencent/ncnn.git ../deps/ncnn
git -C ../deps/ncnn checkout c15904bb8918187ec100f1ebe708942af83b84be
cmake -S ../deps/ncnn -B ../deps/ncnn/build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/../deps/ncnn-install" -DNCNN_VULKAN=OFF \
  -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_TOOLS=OFF \
  -DNCNN_BUILD_TESTS=OFF -DNCNN_BUILD_BENCHMARK=OFF
cmake --build ../deps/ncnn/build -j2
cmake --install ../deps/ncnn/build
bash scripts/fetch_ncnn_demo.sh
./build/veg_generate_person_fixture data/people_demo.jpg data/people_demo.avi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNCNN_ROOT="$PWD/../deps/ncnn-install" \
  -DNCNN_SAMPLE_IMAGE="$PWD/data/people_demo.jpg" \
  -DNCNN_MODEL_PARAM="$PWD/data/models/yolov8n.param" \
  -DNCNN_MODEL_BIN="$PWD/data/models/yolov8n.bin"
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/veg_gateway --config config/gateway-ai.yaml --max-frames 12
```

此样例用静止人物画面检验检测、轨迹、区域进入及滞留链路；它**不证明真实动作识别能力**。`config/gateway-ai.yaml` 中 `class_id: 0` 是 COCO 人员类。要做安全帽、跌倒等违规行为，需要对应数据集、模型与规则，并在真实视频上评估误报和漏报。

可重复测量此样例的峰值进程内存和处理耗时：

```bash
python3 scripts/benchmark_gateway.py --gateway build/veg_gateway \
  --config config/gateway-ai.yaml --max-frames 12
```

当前 Ubuntu 虚拟机上，该 12 帧样例的进程峰值 RSS 为约 168 MiB，平均每帧处理约 64 ms。这是单路、832×416、静止画面、CPU ncnn 的样例测量，不能外推成实际摄像头或 RK3588 的容量指标。

## 接口与配置

`GET /health` 返回每路 `state`、处理帧数、事件数、重连次数、最近帧距今的毫秒数及平均处理毫秒数。`GET /api/v1/events` 返回每路最近 200 条内存事件，可用 `camera_id` 过滤。完整事件保存在 JSONL。`GET /metrics` 返回 `veg_frames_total`、`veg_events_total`、`veg_reconnects_total` 和 `veg_processing_microseconds_total`，均带 `camera` 标签。

配置示例见 `config/gateway.yaml` 与 `config/gateway-ai.yaml`。`zone` 和 `line_x` 是相对于画面尺寸的坐标；`dwell_frames` 是目标进入区域后累计的帧数。`detector` 可取 `motion` 或 `ncnn_yolov8`。模型文件路径相对于配置文件所在目录；视频文件 URI 当前相对于进程工作目录。HTTP 默认监听全部网卡，当前没有认证，测试环境之外应放在受控网络或加反向代理认证。

## 项目结构与验证

| 路径 / 程序 | 职责 |
| --- | --- |
| `include/veg/core.hpp`、`src/core.cpp` | 运动检测、简单目标关联、事件规则 |
| `src/gateway.cpp`、`veg_gateway` | 多路采集、ncnn 接入、存证、HTTP API、指标 |
| `src/rtsp_server.cpp`、`veg_rtsp_server` | 测试 RTSP/H.264 发布 |
| `src/main.cpp`、`veg_detect` | 单路检测命令行，实时输入使用有界帧队列 |
| `tests/` | 规则、生成录像、RTSP 回环、双路网关和 ncnn 端到端测试 |

虚拟机中已通过 5 项 CTest：规则、合成视频、RTSP 回环、双路网关、ncnn 人员检测网关。测试基于合成越线视频及示例人物图片，不代表真实摄像头场景精度。当前目标关联是质心距离贪心匹配，多人遮挡可能换 ID；网关每路采集与处理串行，尚未为多路 AI 设置限速或有界队列。详见 [工程设计与后续计划](docs/design.md)。

## 参考项目及取舍

- [Drogon](https://github.com/drogonframework/drogon)：网关 HTTP API。
- [ncnn](https://github.com/Tencent/ncnn)：可选的 CPU 推理后端，未来可替换为 RKNN。
- [ZLMediaKit](https://github.com/ZLMediaKit/ZLMediaKit) 和 [SRS](https://github.com/ossrs/srs)：参考流媒体服务的分层与运维设计；当前测试发布端使用 GStreamer RTSP Server，并未直接集成这两个服务。
- [MediaPipe](https://github.com/google-ai-edge/mediapipe)：参考检测、跟踪、事件规则之间的可替换处理链设计；当前项目未依赖其运行库。

RK3588 + 8 GB 内存是合理的后续目标，但可同时处理的路数取决于输入分辨率、帧率、硬件解码、模型和推理后端。先在真实硬件上测峰值 RSS、解码占用、单帧推理耗时和端到端延迟，再确定路数。当前没有对 RK3588 的性能结论。
