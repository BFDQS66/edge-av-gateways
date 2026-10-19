# 中英离线语音翻译的模型选择

## 当前实施路线

项目现在需要包含**本地大模型翻译**，因此先在 3.8 GiB Ubuntu 虚拟机中验证 [Qwen3 1.7B Q4_K_M](https://huggingface.co/ggml-org/Qwen3-1.7B-GGUF)，用 [llama.cpp 的本地 HTTP 服务](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)推理，并通过 C++ 网关串接 STT。模型量化文件约 1.28 GB。该配置是虚拟机可运行的工程基线，不代表香橙派 5 Pro 的最终模型大小或速度；16 GB 板端可再比较更大的量化模型与 NPU/CPU 后端，必须以音视频管线同时运行的实测为准。安装和接口见[本地 LLM 部署](translation_deployment.md)。

下一步仍应建立**专用机器翻译模型对照**：中文→英文测试 [Helsinki-NLP/opus-mt-zh-en](https://huggingface.co/Helsinki-NLP/opus-mt-zh-en)，英文→中文测试 [Helsinki-NLP/opus-mt-en-zh](https://huggingface.co/Helsinki-NLP/opus-mt-en-zh)。它们是 Marian/OPUS-MT 路线，可用 [CTranslate2 的 MarianMT 转换和推理流程](https://opennmt.net/CTranslate2/guides/transformers.html)部署，运行时有 C++ API。固定语言对和短句场景可能更适合专用模型的内存与延迟；跨句上下文、术语或风格控制则是 LLM 可能提供价值的地方。最终选择要靠同一语料、同一硬件上的质量、P95 和内存结果，而不是只看模型类别。

不要让 LLM 根据模糊的 ASR 文本擅自补出未说过的内容；数字、姓名、地名与否定词要单独核验。量化、线程数、上下文长度和输出 token 上限都影响资源与质量；[CTranslate2 性能建议](https://opennmt.net/CTranslate2/performance.html)也指出专用模型的 INT8、线程和 beam size 需要权衡。

两份 OPUS-MT 模型卡的许可证不同：中文→英文为 CC-BY-4.0，英文→中文标为 Apache-2.0。英文→中文模型卡还要求目标语言前缀，例如简体中文 `>>cmn_Hans<<`；转换和 C++ 分词器集成时需显式验证输出目标语种。具体模型质量仍需在本项目语料上实测。

[NLLB-200 distilled 600M](https://huggingface.co/facebook/nllb-200-distilled-600M)可作为多语言研究对照，[CTranslate2 也给出了转换示例](https://opennmt.net/CTranslate2/guides/transformers.html)，但该模型卡标为 CC-BY-NC-4.0，且 600M 模型带来更大的资源开销；因此不作为当前双语、面向求职作品的默认部署选型。

## 验证方法

1. 用**人工参考原文**输入翻译模型，单独测翻译质量：chrF、SacreBLEU、领域术语与数字保留率，并抽样人工检查是否漏译或添加事实。
2. 再用 **STT 实际输出**输入同一翻译模型，测整条链路的译文质量；两组结果的差距显示语音识别错误传播的影响。
3. 测 STT、翻译和 TTS 各自以及整条链路的 P50/P95、吞吐量、当前/峰值 RSS；在与服务同机并发时重测。
4. 固定数据、模型版本、分词器版本、INT8 转换参数和随机种子。模型上线以领域语料上的质量与资源预算为准，不能只根据模型卡的公开测试分数决定。

当前链路为 `WAV → VAD → STT → Qwen3 翻译 → Piper TTS`；后续再增加专用翻译模型作为同条件对照。小规模冒烟样例只用于发现明显的数字、否定词和格式错误；正式质量结论需要双语参考语料和人工审阅。
