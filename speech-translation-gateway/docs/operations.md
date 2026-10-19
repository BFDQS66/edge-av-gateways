# 常驻语音服务：接口、限制与运维

虚拟机项目在 `/home/fu-shanfeng/workspace/speech-translation-gateway`。`speech_server` 是 C++ 常驻进程，启动时一次加载 SenseVoice INT8、Silero VAD 和两套 Piper 音色；服务只绑定 `127.0.0.1:8765`。`deploy/speech-gateway.service` 已安装为用户级 systemd 服务并启用，`loginctl` 的 linger 也已启用，所以用户退出登录后仍可运行、开机时由用户服务管理器启动。没有 TLS 或身份认证，访问远端应使用 SSH 转发，不要直接暴露端口。

构建完成后可用 `bash scripts/deploy_user_service.sh` 重新安装服务单元、启用 timer 和 linger，并等待 `/readyz` 就绪。

## 接口

| 方法与路径 | 输入 | 输出 |
| --- | --- | --- |
| `GET /healthz` | 无 | 进程存活 |
| `GET /readyz` | 无 | 模型已加载，监听就绪 |
| `GET /metrics` | 无 | 运行时长、请求/成功/错误/忙计数、最近请求延迟、当前与峰值 RSS、ASR 后端 |
| `POST /v1/stt` | `audio/wav`，单声道 PCM16 WAV | JSON：文本、音频/语音时长、语音片段数、请求处理时间 |
| `POST /v1/tts/zh` | `text/plain; charset=utf-8` | 中文 Piper 输出 `audio/wav` |
| `POST /v1/tts/en` | `text/plain; charset=utf-8` | 英文 Piper 输出 `audio/wav` |

STT 接受 8～48 kHz、最长 30 秒、最多 8 MiB 的 WAV；内部转为 16 kHz。文本最多 500 UTF-8 字节。输入无效返回 400/413；推理锁被占用时快速返回 503，调用方应延迟重试。HTTP 工作线程上限 2、等待队列上限 8，读取超时 10 秒，写入超时 30 秒。所有模型推理目前串行执行，避免在 4 vCPU 虚拟机上争抢内存和 CPU。此配置是稳定性优先的单实例基线，吞吐能力需按实际负载重新测。

默认 STT 策略是 `gate`：VAD 检测到语音后，将完整音频交给 STT；纯静音不进入 STT。测试参数 `?strategy=segments|gate|off` 用于比较切段识别、门控识别和不启用 VAD，实际业务使用默认值。`off` 在纯静音上会误转写，不适合作为默认模式。

虚拟机内调用示例：

```bash
cd ~/workspace/speech-translation-gateway
python3 - <<'PY'
from pathlib import Path
from urllib.request import Request, urlopen

audio = Path('models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/test_wavs/zh.wav').read_bytes()
request = Request('http://127.0.0.1:8765/v1/stt', audio,
                  {'Content-Type': 'audio/wav'})
with urlopen(request, timeout=30) as response:
    print(response.read().decode())
PY
```

从 Windows 访问可先建立 SSH 转发：`ssh -F none -L 8765:127.0.0.1:8765 fu-shanfeng@192.168.222.133`，然后访问 Windows 本机的 `127.0.0.1:8765`。

## 管理命令

```bash
systemctl --user status speech-gateway.service
systemctl --user restart speech-gateway.service
journalctl --user -u speech-gateway.service -n 50 --no-pager
loginctl show-user fu-shanfeng -p Linger
```

服务单元设置 `Restart=on-failure`、3 秒重启延迟、每分钟最多 5 次启动、`MemoryMax=1500M`、`TasksMax=64`、文件描述符上限 1024。模型加载约需数秒；状态显示 `active` 后仍应等待 `/readyz` 返回 200。进程异常退出后请求可能暂时失败，客户端应有有限次数重试。日志在用户级 journal 中。

`speech-gateway-health.timer` 每分钟运行一次健康探测：若 `/metrics` 连续无法访问，或一项推理持续超过 45 秒，便重启服务。检查方式：`systemctl --user status speech-gateway-health.timer`、`journalctl --user -u speech-gateway-health.service -n 20 --no-pager`。常规请求的 503 不会触发重启。

## 已执行的稳定性验证

- 300 次顺序混合 STT、静音、TTS 请求全部成功；8 并发突发请求有 1 个成功、7 个立即返回 503，服务随后仍就绪。
- 在同一进程再执行 600 次顺序请求，全部成功；已预热 RSS 从 554.98 MiB 到 555.17 MiB，增长 0.19 MiB；本轮本机 HTTP P95 为 252 ms。报告：`output/soak/report_600.json`。
- 错误 WAV、无效 UTF-8 和过长文本分别返回受控的 400/413；随后健康检查正常。
- 对 systemd 管理的进程发送 `SIGKILL` 后，`NRestarts=1`，约 8 秒后新进程的 `/readyz` 返回 200。
- 人为暂停进程使其仍存活但不响应 HTTP；健康探测连续超时后执行重启，约 5 次每秒就绪轮询后新进程返回 200。
- 3000 次混合请求在约 577 秒内全部成功；测试期间另有较重的 ASR 对照实例运行。HTTP 请求 P95 约 380 ms，峰值 RSS 约 559 MiB。冷进程到测试结束的当前 RSS 增长约 78 MiB，包含首次 TTS 缓存分配。

这些测试验证了短时间重复请求、异常输入和进程自动恢复。它们不等于通过 24 小时或更长的可靠性验收；正式验收还需模拟音频长度分布、持续并发、磁盘满、内存压力、系统重启及网络中断，并设置错误率、端到端 P95、内存增长和恢复时间的阈值。

下一轮长期验收建议固定语料和发送速率，连续运行至少 24 小时；把正常请求的 5xx 比例、显式过载 503 比例、各接口 P50/P95、每小时 RSS、进程重启次数记录下来。拟定门槛：无非预期崩溃；无负载时 5xx 为零；预热后 RSS 增长不超过 50 MiB；人为杀死进程后 30 秒内恢复就绪，模拟卡住后 90 秒内恢复。当前约 10 分钟的 3000 请求测试只覆盖其中一部分，门槛尚未通过 24 小时验证。
