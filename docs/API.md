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

每进程只支持一个识别实例。创建/销毁由调用方串行管理，不得与其他使用同一个实例的调用并发；重复创建或并发识别返回 `LINGASR_BUSY`。识别应运行在 core 1、栈至少 **20480 字节**的任务中，SDK 自建 core 0 数学工作任务。取消回调可能在两个内核调用，必须线程安全、快速、不阻塞，不得销毁实例或重入 SDK。

`lingasr_transcribe()` 同步返回最终文字。返回值包括 INVALID_ARG、NO_MEMORY、MODEL_MISMATCH、BUSY、CANCELLED 和 INFERENCE_FAILED；失败时不使用输出文字。`lingasr_get_memory()` 返回激活、内部工作区、解码及私有 PCM 缓冲区分配大小，不等于整个应用的总内存。

`lingasr_capture.h` 提供独立的不透明采集状态。每次 `push` 输入 320 样本（20 ms），输出区须有 128000 样本容量。READY 后读取 info 并复制有效 PCM，然后 `advance`；先复制再推进以保留保护切分的尾部。预热阶段可用 `observe` 校准噪声，`cancel` 保留噪声学习状态。各 capture 实例不可并发使用。

`lingasr_audio_queue.h` 是应用侧四槽音频队列协议；调用方分配缓冲区并加锁，详见助手示例。应用可自行接入 I2S、麦克风、显示和网络，识别库本身不依赖 Wi-Fi 或 LLM。

ABI 版本 1，public version 0.1.0-preview.1。Preview 阶段接口及资源格式可能随版本变化，升级时应成套更新库、资源和公开头文件。

## 助手运行状态

助手示例的 `GET /api/status` 返回 `heap.internal` 和 `heap.psram`，分别提供以字节计的堆总容量、当前空闲量、历史低水位和最大连续空闲块：`total_bytes`、`free_bytes`、`minimum_free_bytes`、`largest_free_block_bytes`。内部堆统计使用 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`；PSRAM 使用 `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`。这些读数描述运行期可分配内存，与 Flash 分区剩余容量不同。

状态中的 `clock_required` 表示当前构建是否要求校时；它由证书日期检查配置决定。为 `false` 时，`clock_ready=false` 不妨碍 LLM 请求。证书链和主机名检查沿用 TLS 配置。堆的 `minimum_free_bytes` 为分配器历史统计，结合 `free_bytes` 的运行期采样评估余量。
