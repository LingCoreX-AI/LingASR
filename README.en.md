<p>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/assets/lingcorex-lockup-h-dark.svg">
    <img src=".github/assets/lingcorex-lockup-h-light.svg" width="180" alt="LingCoreX">
  </picture>
</p>

# LingASR Preview: Offline Chinese Speech Recognition for ESP32-S3

English | [简体中文](README.md)

**Offline Chinese speech recognition on ESP32-S3.**

LingASR converts microphone audio into Chinese text locally on ESP32-S3. Recognition requires no cloud ASR service, making it suitable for voice input, device control and voice assistants.

LingASR Preview is distributed as a precompiled SDK with INT8 deployment resources. Personal noncommercial projects, education/research and internal corporate evaluation are free under the included license. Examples and integration files are MIT; the inference library and models have a separate license. See [Licensing](#licensing).

## What you can build

- **Local transcription:** 16 kHz mono audio to punctuated Chinese text for an LCD or your application.
- **Voice assistant:** local ASR, an online Chat Completions service and local [LingTTS](https://github.com/LingCoreX-AI/LingTTS) speech output.
- **Tool calling:** RGB control, custom HTTP tools, conversation compaction and memory. Native search requires provider and model support.

## Try it on a board without a development environment

Use an ESP32-S3 N16R8, an INMP441 microphone, a MAX98357A amplifier and a speaker. The complete assistant also uses an ST7789 240×240 LCD and a WS2812 RGB LED by default. See [hardware and wiring](docs/HARDWARE.md).

1. Download `lingasr-preview-assistant-0.1.0-preview.1-n16r8.tar.gz` from [Releases](https://github.com/LingCoreX-AI/LingASR/releases) and extract it.
2. Flash the board:

   ```bash
   python -m pip install esptool==4.12.0
   python flash.py --port COM5          # Linux / WSL: python3 flash.py --port /dev/ttyUSB0
   ```

   The script verifies all file checksums and asks you to type YES before flashing.
3. Connect your phone or computer to `LingASR-Assistant`, password `lingasr1234`, and open <http://192.168.4.1/>.
4. In connection settings, scan for or enter your Wi-Fi network. Set the LLM Base URL, Model ID and API Key. Multiple Wi-Fi networks can be saved and selected automatically when available.
5. Once connected, the LCD shows the ready message. The microphone takes about eight seconds to warm up after startup. Speak to see a transcript, receive an LLM response and hear it through local TTS. Listening resumes after playback. Default playback volume is 50%.

Press BOOT briefly to pause/resume listening; hold it for about two seconds to enter hotspot configuration. RST restarts the board and retains configuration in NVS. The configuration page has no user authentication and is intended for a trusted local network.

## Use it in your project

This repository provides examples, public headers, component integration files and documentation. Precompiled libraries and deployment resources are distributed through versioned SDK packages.

After cloning, use Python 3.12+ to download the pinned SDK and verify its SHA-256 checksum:

```bash
python tools/get-sdk.py
```

Alternatively, download and extract `lingasr-preview-sdk-0.1.0-preview.1.tar.gz` from [Releases](https://github.com/LingCoreX-AI/LingASR/releases). It already contains the required components and resources.

With **ESP-IDF v5.5.6** configured, build the offline microphone example:

```bash
idf.py -C examples/microphone -B build/microphone build
idf.py -C examples/microphone -B build/microphone -p COM5 flash monitor
```

This example needs no Wi-Fi, LLM or TTS. After microphone warm-up, press BOOT to record four seconds and print the transcript over UART. The full assistant is in `examples/voice_assistant`; its GPIO settings are available through `idf.py menuconfig`. On Linux / WSL, replace COM5 with the actual device, such as `/dev/ttyUSB0`.

Applications create an engine with `lingasr_create()` and pass PCM to `lingasr_transcribe()` to receive UTF-8 text. Task-stack requirements, resource partitions, cancellation and error codes are documented in the [API reference](docs/API.md).

## Voice assistant

The example sends locally transcribed text to a configured Chat Completions service, then speaks the reply with LingTTS. Its system prompt asks the assistant to interpret plausible homophone transcription errors using context, reply briefly and ask for clarification when needed.

The configuration page has three tabs:

- **Conversation:** transcripts and replies, text-based LLM testing, memory and listening controls.
- **Connection settings:** Wi-Fi scanning and multiple saved networks, plus LLM endpoint and model configuration.
- **Tools:** RGB, search and memory controls, and up to four custom GET / POST HTTP tools. Native search requires provider support; ordinary Chat Completions models do not acquire search automatically.

ASR and TTS run locally. LLM conversations and search require internet access. The example sends transcripts, conversation and tool data to configured endpoints; it does not upload raw audio.

## Specifications

| Item | Specification |
|---|---|
| Hardware | ESP32-S3 N16R8, 16 MiB Flash, 8 MiB octal PSRAM, 240 MHz |
| Development environment | ESP-IDF v5.5.6 |
| Audio input | 16 kHz, signed 16-bit, mono PCM |
| Output | UTF-8 Chinese text with punctuation |
| Recognition | Segmented batch inference, up to eight seconds per segment |
| Voice detection | WebRTC VAD with an adaptive energy gate; seeks a suitable pause after four seconds, with protection cuts and joining when needed |
| ASR deployment resources | Approximately 11.94 MB, including recognition, frontend and decoding resources |
| Assistant components | LingASR Preview, LingTTS 0.2.0-rc2 and a user-configured online LLM |

Background noise, distant speech and microphone installation affect detection and segmentation. See [hardware and wiring](docs/HARDWARE.md) for display and amplifier settings.

## Licensing

| Content | License |
|---|---|
| Examples, public headers and component integration files | [MIT](licenses/MIT.txt) |
| LingASR precompiled engine, deployment models and LingASR core within firmware | [LingASR Preview License 1.0](licenses/LingASR-Preview-License-1.0.md) |
| Bundled LingTTS engine, models and dictionary | [LingTTS Community License 1.0](licenses/LingTTS-Community-License-1.0.md) |

- **Free:** personal noncommercial projects, education/research and internal corporate R&D, testing and prototype demonstrations. No registration, online activation, device-count limit or recognition-time limit.
- **Commercial deployment requires written authorization:** product sales, paid services and commercial production use.
- **Third-party components:** retain their respective licenses; see NOTICE and the included license texts.

The core libraries and models are proprietary; the entire SDK is not MIT-licensed. Commercial licensing and engineering cooperation: service@lingcorex.cn.

## Feedback

For transcription issues, board integration problems or documentation feedback, open an [Issue](https://github.com/LingCoreX-AI/LingASR/issues). Include the board model, SDK version, audio description, expected/actual transcript and relevant UART logs. Do not include API keys, Wi-Fi passwords or private personal data.

---

© 2026 瓴枢智云（上海）科技有限公司 / LingCoreX
