# Ubuntu 虚拟机本地 LLM 翻译实测

测试时间：2026-10-15。环境：Ubuntu 24.04 x86_64、4 vCPU、3.8 GiB RAM。模型为 Qwen3 1.7B Q4_K_M，llama.cpp CPU 推理，2 线程、单槽、2048 token 上下文；C++ 翻译网关调用本地 llama.cpp 服务。安装版本、哈希和接口见[翻译部署](translation_deployment.md)。这些数字**不是香橙派 5 Pro 的预测值**。

## 文本翻译冒烟测试

先预热 1 条，再运行 8 条手工中英双向短句，覆盖摄像头、设备编号、告警、服务、时间、否定词和开发板。8 条全部产生目标语言译文；客户端报告平均翻译处理时间约 1282 ms，P95 约 1916 ms。含数字的 4 条均保留了原文中的阿拉伯数字形式。样本太小，参考译文并非唯一答案，因此不报告 BLEU、chrF 或正式错误率。

首次提示词把硬件语境中的 “board” 译为“棋盘”；加入嵌入式硬件语境后输出“该板”，避免了明显误义，但还没有稳定输出期望术语“开发板”。前后原始结果分别在 `output/evaluation/translation_smoke_before_domain.json` 和 `output/evaluation/translation_smoke.json`。后续应提供术语表、实际对话和人工参考译文，再决定是否增加受控的术语约束。

## 两段公开录音的全链路

通过 `scripts/voice_translate.py` 调用现有 SenseVoice STT、Qwen3 翻译和 Piper TTS，生成目标语言 WAV：

| 输入 | STT 结果中的错误 | 译文 | STT / LLM / 端到端 | 目标 WAV |
| --- | --- | --- | ---: | ---: |
| 中文官方样本 | “开放时间”识别成“开饭时间” | “Breakfast time from 9:00 a.m. to 5:00 p.m.” | 2689 / 2063 / 5790 ms | 英文 3.855 s |
| 英文官方样本 | “gold”识别成“code” | “部落酋长召见男孩，并给予他50份代码。” | 722 / 2106 / 3449 ms | 中文 3.970 s |

这两条展示了**识别错误向翻译传播**。翻译模型按收到的错误文本工作；不能用提示词擅自把听不清的词改成推测内容。第一条 STT 延迟较高，随后复测回落，需在更长时间与板端负载下观察换页影响。生成音频分别保存于 `output/voice_translate/zh_to_en/translated_en.wav`、`output/voice_translate/en_to_zh/translated_zh.wav`。

## 稳定性和资源

- 50 次预热后的中英交替翻译请求全部成功；客户端平均约 972 ms，P95 约 1317 ms。4 个并发请求中 1 个成功、3 个快速返回受控的 503。模型进程 RSS 从约 1394 MiB 到 1408 MiB（+14.5 MiB），语音进程约 547 MiB、C++ 网关约 4.8 MiB 基本不变。报告：`output/soak/translation.json`。
- 强制结束 llama.cpp 进程后，systemd 重启并在约 9.3 秒内恢复 `/readyz`；强制结束翻译网关后约 4.1 秒恢复。随后再次运行中英翻译成功。报告：`output/soak/translation_recovery.json`。
- LLM 常驻期间，原语音服务的 100 次混合 STT/TTS 请求全部成功，客户端 HTTP P95 约 269 ms；8 并发突发请求中 1 个成功、7 个受控 503。报告：`output/soak/report_100.json`。
- 错误 JSON、空文本、同语种翻译、超长请求、损坏的 WAV 均被拒绝；随后 `/readyz` 仍返回正常。模型服务、网关和健康定时器均已启用，且每分钟探测日志正常。
- 重复执行 llama.cpp 安装脚本和翻译服务部署脚本后，模型重新启动并再次完成 8 条双向翻译；说明当前虚拟机的部署步骤可重复执行。

这台 3.8 GiB 虚拟机在模型和语音服务同时运行时，交换分区占用曾从部署前约 195 MiB 上升到约 500 MiB；不能把当前短句延迟直接套用到 RK3588S。以上验证仍不等于 24 小时稳定性或领域翻译质量验收。上板后需要在音视频、STT、LLM、TTS 同时运行时重测 P95、RSS、交换分区、功耗和温度，并用真实双语语料评估数字、否定、专名和术语。
