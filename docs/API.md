# SDK API

使用 ESP-IDF v5.5.6，将 `components/lingasr` 放入工程，并调用 `lingasr_flash_assets()` 为 `asr_model`、`asr_front`、`asr_tokens`、`asr_backend` 分区注册资源。资源须与本版本 SDK 成套使用。分区大小和地址示例见示例的 partitions.csv。

```c
#include "lingasr.h"
lingasr_engine_t *engine = NULL;
lingasr_config_t config;
lingasr_default_config(&config);
lingasr_status_t status = lingasr_create(&config, &engine);
if (status == LINGASR_OK) {
    char text[1024];
    lingasr_request_t request = {.pcm = pcm, .samples = sample_count};
    lingasr_result_t metrics;
    status = lingasr_transcribe(engine, &request, text, sizeof(text), &metrics);
    /* Successful output is UTF-8; metrics reports microseconds for each stage. */
    lingasr_destroy(engine);
}
```

`pcm` 是 16 kHz 单声道、有符号 16 位 PCM，长度 1..128000 样本。`window_ms=0` 自动选择四至八秒的计算窗口，也可指定 4000、5000、6000、7000、8000，输入长度不能超过窗口。SDK 复制输入并补零，调用期间保持原缓冲区不变。输出缓冲区至少 1024 字节。

每进程只支持一个识别实例。创建/销毁由调用方串行管理，不得与其他使用同一个实例的调用并发；重复创建或并发识别返回 `LINGASR_BUSY`。识别任务栈至少 **20480 字节**。默认双核模式下，识别任务须绑定 core 1，SDK 自建 core 0 数学工作任务。取消回调可能在两个内核调用，必须线程安全、快速、不阻塞，不得销毁实例或重入 SDK。

`lingasr_transcribe()` 同步返回最终文字。返回值包括 INVALID_ARG、NO_MEMORY、MODEL_MISMATCH、BUSY、CANCELLED 和 INFERENCE_FAILED；失败时不使用输出文字。`lingasr_get_memory()` 返回激活、内部工作区、解码及私有 PCM 缓冲区分配大小，不等于整个应用的总内存。

`lingasr_capture.h` 提供独立的不透明采集状态。每次 `push` 输入 320 样本（20 ms），输出区须有 128000 样本容量。READY 后读取 info 并复制有效 PCM，然后 `advance`；先复制再推进以保留保护切分的尾部。预热阶段可用 `observe` 校准噪声，`cancel` 保留噪声学习状态。各 capture 实例不可并发使用。

`lingasr_audio_queue.h` 是应用侧四槽音频队列协议；调用方分配缓冲区并加锁，详见助手示例。应用可自行接入 I2S、麦克风、显示和网络，识别库本身不依赖 Wi-Fi 或 LLM。

ABI 版本 1，public version 0.1.0-preview.2。Preview 阶段接口及资源格式可能随版本变化，升级时应成套更新库、资源和公开头文件。

## 单核与双核推理

```c
lingasr_engine_t *engine = NULL;
lingasr_status_t status = lingasr_create_ex(NULL, LINGASR_SINGLE_CORE, &engine);
if (status == LINGASR_OK) {
    /* Single-core inference runs on this task. */
    /* When idle, enable the helper; subsequent transcribe calls use core 1. */
    status = lingasr_set_execution(engine, LINGASR_DUAL_CORE);
    lingasr_execution_mode_t mode;
    if (status == LINGASR_OK) status = lingasr_get_execution(engine, &mode);
    lingasr_destroy(engine);
}
```

`lingasr_create()` 保持默认双核行为。`create_ex()` 可在创建时选择 `LINGASR_SINGLE_CORE` 或 `LINGASR_DUAL_CORE`；单核模式不会创建数学工作任务，也不会为其分配 6144 字节栈、任务控制块、队列或同步信号量。单核仍使用 SIMD/INT8 优化，前端、识别和解码都由调用任务执行。

`set_execution()` 在空闲时切换模式；识别期间返回 `LINGASR_BUSY`。切到双核需要分配工作任务，内存不足返回 `LINGASR_NO_MEMORY` 并保持原模式可用。切到单核删除工作任务；FreeRTOS 的任务内存回收可能稍后由空闲任务完成。`get_execution()` 返回配置的执行模式，识别期间也返回 `LINGASR_BUSY`。

此 API 控制 ASR 的计算并行方式。Wi-Fi、音频、TTS 和系统任务的核绑定仍由应用和 ESP-IDF 管理，API 不改变调用任务的核绑定，也不切换 ESP-IDF 的 `CONFIG_FREERTOS_UNICORE`。双核模式要让识别调用任务绑定 core 1，才能与 core 0 工作任务并行；SDK 不自动迁移调用任务。

## 内置 VAD 参数

```c
#include "lingasr_capture.h"
lingasr_capture_config_t config;
lingasr_capture_default_config(&config);
config.end_silence_ms = 600;
config.split_after_ms = 0; /* Wait for sentence end; retain maximum-length guard. */
lingasr_capture_t *capture = NULL;
lingasr_status_t status = lingasr_capture_create_ex(&config, &capture);
```

| 参数 | 默认值 | 范围和含义 |
| --- | --- | --- |
| `vad_source` | `LINGASR_VAD_BUILTIN` | 内置 WebRTC 或 `LINGASR_VAD_EXTERNAL` |
| `vad_mode` | 2 | WebRTC 0..3，值越大判定越严格 |
| `end_silence_ms` | 800 | 连续非人声多久收句；20..`max_segment_ms` |
| `split_after_ms` | 4000 | 达到该片段时长后允许在停顿处切分；0 关闭，最大 `max_segment_ms` |
| `split_silence_ms` | 160 | 上述切分需要的连续非人声时长；20..`end_silence_ms` |
| `max_segment_ms` | 8000 | 保护切分上限；1000..8000 |
| `preroll_ms` | 300 | 起声前缓存及保护切分上下文；0..300，起声触发帧始终保留 |
| `min_speech_ms` | 100 | 片段内累计有效人声至少达到该时长；20..`max_segment_ms` |
| `onset_window_ms` | 120 | 起声判定滑动窗口；20..640 |
| `onset_speech_ms` | 80 | 窗口内人声帧累计时长；20..`onset_window_ms`，无需连续 |
| `min_rms` | 100 | 绝对 AC 能量门限；0..32767，0 关闭此门限 |
| `noise_ratio` | 4 | 相对背景能量倍数；有限数 0..64，0 关闭此门限 |

所有时长须为 20 ms 的整数倍。内置 VAD 需同时通过 WebRTC、绝对门限和相对背景门限；录音中使用较低的能量保持门限。改变门限只影响内置 VAD。较短收句停顿会降低等待时间，也更容易将句中停顿切开。

`lingasr_capture_create()` 和 `create_ex(NULL, ...)` 使用默认参数。配置结构先调用 `default_config()` 初始化，包括 `struct_size`。`get_config()` 写出当前配置。`set_config()` 仅在空闲状态可用；正在采集或等待消费 READY 时返回 `LINGASR_BUSY`，应先消费并 `advance()`，或明确 `cancel()` 后再改。成功设置会清除起声缓存、WebRTC 和噪声学习状态，必要时重新预热；无效参数返回 `LINGASR_INVALID_ARG` 并保留原状态。

到达保护上限时，会在末尾 800 ms 内寻找低能量位置切分，READY 的有效长度可能小于 `max_segment_ms`。`advance()` 保留未提交的尾部及配置的上下文。输出缓冲区仍须分配 128000 样本。

## 接入自己的 VAD

```c
lingasr_capture_config_t config;
lingasr_capture_default_config(&config);
config.vad_source = LINGASR_VAD_EXTERNAL;
lingasr_capture_t *capture = NULL;
lingasr_status_t status = lingasr_capture_create_ex(&config, &capture);
/* Check status before entering the audio loop. */

/* For each complete 320-sample frame, in chronological order: */
bool is_speech = my_vad(frame); /* Caller-provided classifier. */
int event = lingasr_capture_push_vad(capture, frame, is_speech, pcm);
if (event == LINGASR_CAPTURE_READY) {
    lingasr_capture_info_t info;
    lingasr_capture_get_info(capture, &info);
    /* Copy pcm[0..info.samples), or transcribe synchronously, before advance. */
    lingasr_capture_advance(capture, pcm);
}
```

外部模式直接接受 `is_speech`，不调用内部 WebRTC，也不再叠加内部能量门限。起声投票、短片段丢弃、停顿收句、切分和尾部保留仍由 SDK 执行。**非人声帧也必须按顺序送入**，否则停顿计时无法推进。若外部 VAD 已经提供稳定起声，可将 `onset_window_ms` 和 `onset_speech_ms` 均设为 20，避免重复起声等待。

外部模式的 `observe()` 不做处理。外部模式调用普通 `push()`、内置模式调用 `push_vad()`，均返回 `LINGASR_CAPTURE_ERROR`；不会悄悄混用判定来源。每个 capture 实例的所有操作须由调用方串行执行。

若调用方已经完成整句分段，也可跳过 capture，直接用 `lingasr_transcribe()` 提交 PCM。

## 助手运行状态

助手示例的 `GET /api/status` 返回 `heap.internal` 和 `heap.psram`，分别提供以字节计的堆总容量、当前空闲量、历史低水位和最大连续空闲块：`total_bytes`、`free_bytes`、`minimum_free_bytes`、`largest_free_block_bytes`。内部堆统计使用 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`；PSRAM 使用 `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`。这些读数描述运行期可分配内存，与 Flash 分区剩余容量不同。

状态中的 `clock_required` 表示当前构建是否要求校时；它由证书日期检查配置决定。为 `false` 时，`clock_ready=false` 不妨碍 LLM 请求。证书链和主机名检查沿用 TLS 配置。堆的 `minimum_free_bytes` 为分配器历史统计，结合 `free_bytes` 的运行期采样评估余量。
