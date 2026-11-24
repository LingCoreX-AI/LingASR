<p>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/assets/lingcorex-lockup-h-dark.svg">
    <img src=".github/assets/lingcorex-lockup-h-light.svg" width="180" alt="瓴枢智云 LingCoreX">
  </picture>
</p>

# LingASR Preview：ESP32-S3 离线中文语音识别

[English](README.en.md) | 简体中文

**ESP32-S3 上的离线中文语音识别。** Offline Chinese speech recognition for ESP32-S3.

LingASR 是一款面向端侧的中文语音识别模型，在 ESP32-S3 上将麦克风采集的语音转成文字。识别过程在本地完成，无需云端 ASR 服务，可用于语音输入、设备控制和语音助手。

LingASR Preview 通过预编译 SDK 和 INT8 部署资源分发。个人非商业项目、教学研究及公司内部研发评估免费；示例代码 MIT，核心库和模型适用独立许可，见[授权](#授权)。

## 可以做什么

- **本地语音转写**：16 kHz 单声道语音输入，输出带标点的中文文字，可显示到 LCD 或送入自己的业务逻辑。
- **语音助手**：示例连接本地 ASR、在线 Chat Completions 和本地 [LingTTS](https://github.com/LingCoreX-AI/LingTTS)，完成听、回复和播报。
- **工具调用**：演示 RGB 灯控制，支持自定义 HTTP 工具、对话压缩及记忆；原生搜索需要模型和服务端支持。

## 上板体验（不装开发环境）

需要 ESP32-S3 N16R8 开发板、INMP441 麦克风、MAX98357A 功放和喇叭。完整助手默认接有 ST7789 240×240 LCD 及 WS2812 RGB，接线见[硬件说明](docs/HARDWARE.md)。

1. 从 [Releases](https://github.com/LingCoreX-AI/LingASR/releases) 下载 `lingasr-preview-assistant-0.1.0-preview.1-n16r8.tar.gz` 并解压。
2. 烧录：

   ```bash
   python -m pip install esptool==4.12.0
   python flash.py --port COM5          # Linux / WSL：python3 flash.py --port /dev/ttyUSB0
   ```

   脚本先核对全部文件的校验值，再让你输入 YES 确认。
3. 手机或电脑连接热点 `LingASR-Assistant`（密码 `lingasr1234`），打开 <http://192.168.4.1/>。
4. 在“连接设置”中扫描或手动输入 Wi-Fi，再填写 LLM 的 Base URL、Model ID 和 API Key。Wi-Fi 可保存多个，下次自动选择已保存的可用网络。
5. 联网后屏幕显示 `ready，等待语音指令`。麦克风启动后约需八秒预热；说话后显示转写，收到 LLM 回复后本地播报，播报结束继续监听。默认音量 50%。

短按 BOOT 暂停或继续监听，长按约两秒进入热点配置；RST 重启并保留配置。参数保存在板端 NVS。配置页用于可信局域网，无用户鉴权。

## 在自己的工程里用

本仓库提供示例代码、公开头文件、组件接入文件和文档；预编译引擎及模型通过版本化 SDK 包分发。

克隆仓库后，用 Python 3.12+ 下载固定版本 SDK 并校验 SHA-256：

```bash
python tools/get-sdk.py
```

也可从 [Releases](https://github.com/LingCoreX-AI/LingASR/releases) 手动下载 `lingasr-preview-sdk-0.1.0-preview.1.tar.gz` 并解压；SDK 包已包含组件和资源，可直接构建。

配置好 ESP-IDF **v5.5.6** 环境后，先编译离线麦克风示例：

```bash
idf.py -C examples/microphone -B build/microphone build
idf.py -C examples/microphone -B build/microphone -p COM5 flash monitor
```

这个示例无需 Wi-Fi、LLM 或 TTS。麦克风预热完成后，短按 BOOT 录制四秒，识别结果显示在串口。完整助手使用 `examples/voice_assistant`，GPIO 可通过 `idf.py menuconfig` 调整。Linux / WSL 把串口替换成 `/dev/ttyUSB0` 等实际设备。

自有应用通过 `lingasr_create()` 创建引擎，调用 `lingasr_transcribe()` 输入 PCM、读取 UTF-8 转写文字。识别任务栈、资源分区、取消和错误码见 [API 参考](docs/API.md)。

## 语音助手

示例将本地识别结果经 Chat Completions 发给配置的 LLM，再由 LingTTS 播报。提示词引导助手按上下文理解可能的同音误转写，简短回复，含义不清时向用户询问。

配置页提供“对话”“连接设置”“工具”三个标签页：

- **对话**：查看转写与回复，可用文字测试 LLM，管理记忆和监听状态。
- **连接设置**：扫描、选择或手动输入 Wi-Fi，保存多个网络；配置 LLM 接口及模型。
- **工具**：管理 RGB、搜索和记忆工具，新增最多四个自定义 HTTP 工具，支持 GET / POST。普通 Chat Completions 模型不会自动获得搜索能力，原生搜索取决于服务端支持。

ASR 和 TTS 在本地运行；LLM 对话和搜索需要联网。示例向配置的接口发送转写文字、对话及工具数据，不上传原始语音。

## 规格

| 项目 | 规格 |
|---|---|
| 目标硬件 | ESP32-S3 N16R8，16 MiB Flash、8 MiB 八线 PSRAM，240 MHz |
| 开发环境 | ESP-IDF v5.5.6 |
| 语音输入 | 16 kHz · 16 bit · 单声道 PCM |
| 识别输出 | UTF-8 中文文字，包含标点 |
| 识别方式 | 分段批量识别，单段最多八秒 |
| 人声监测 | WebRTC VAD 加自适应能量门限；四秒以后寻找合适停顿，必要时保护切分并拼接 |
| ASR 部署资源 | 约 11.94 MB，包含识别模型、前端和解码资源 |
| 助手组件 | LingASR Preview + LingTTS 0.2.0-rc2 + 用户配置的在线 LLM |

环境噪声、远处人声和麦克风安装会影响人声监测与切分。设备接线、屏幕和功放配置见[硬件说明](docs/HARDWARE.md)。

## 授权

| 内容 | 许可 |
|---|---|
| 本仓库的示例代码、公开头文件和组件接入文件 | [MIT](licenses/MIT.txt) |
| LingASR 预编译引擎库、部署模型及体验固件中的 LingASR 核心部分 | [LingASR Preview 许可协议 1.0](licenses/LingASR-Preview-License-1.0.md) |
| 助手附带的 LingTTS 引擎、模型及词典 | [LingTTS 社区许可协议 1.0](licenses/LingTTS-Community-License-1.0.md) |

- **免费**：个人非商业项目、学习教学，以及公司内部研发、测试和原型演示。不用注册，不联网激活，不限设备数量和识别时长。
- **商业部署需书面授权**：销售产品、收费服务、正式商业部署。
- **第三方组件**：沿用各自许可，见 NOTICE 及随包许可文件。

核心库和模型闭源，整个 SDK 不能当作 MIT 使用。商业授权和工程合作：service@lingcorex.cn。

## 反馈

转写问题、跑不起来的板子和文档问题，请在 [Issues](https://github.com/LingCoreX-AI/LingASR/issues) 提交，附上板型、SDK 版本、输入语音描述、预期与实际转写及相关串口日志。请勿附上 API Key、Wi-Fi 密码或个人隐私。

---

© 2026 瓴枢智云（上海）科技有限公司
