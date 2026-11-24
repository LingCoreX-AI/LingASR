# LingASR Preview 0.1.0-preview.2

ESP32-S3 N16R8 上的本地中文 ASR 预览版，支持本地识别、在线 Chat Completions 对话及 LingTTS 播报。

本版新增单核／双核执行控制、内置 VAD 参数配置和外部人声判定输入 API，原有接口默认行为保持一致。

- SDK：`lingasr-preview-sdk-0.1.0-preview.2.tar.gz`，预编译识别库、INT8 部署资源、MIT 接入接口、离线麦克风及完整助手示例。
- 助手体验固件：`lingasr-preview-assistant-0.1.0-preview.2-n16r8.tar.gz`，包含全部镜像、校验清单及烧录脚本。
- 每包提供 `.sha256`。ESP-IDF 固定 v5.5.6，模型修订 zh-preview-r1，ABI 1。

模型和推理核心闭源。个人非商业、教学研究及企业内部研发评估免费，无联网激活；商业部署须书面授权。LingTTS 与第三方组件沿用各自许可。

首次使用连接热点 LingASR-Assistant，密码 lingasr1234，访问 http://192.168.4.1/ 配置 Wi-Fi 和 LLM。接线及使用步骤见包内 README。

