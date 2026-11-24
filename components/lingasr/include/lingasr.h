/* Copyright (c) 2026 LingCoreX. Public integration interface: MIT. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LINGASR_VERSION "0.1.0-preview.1"
#define LINGASR_ABI_VERSION 1
#define LINGASR_SAMPLE_RATE 16000
#define LINGASR_MAX_SAMPLES 128000

typedef struct lingasr_engine lingasr_engine_t;
typedef bool (*lingasr_cancel_fn)(void *userdata);
typedef enum {
    LINGASR_OK=0, LINGASR_INVALID_ARG=-1, LINGASR_NO_MEMORY=-2,
    LINGASR_INVALID_STATE=-3, LINGASR_MODEL_MISMATCH=-4,
    LINGASR_INFERENCE_FAILED=-5, LINGASR_CANCELLED=-6, LINGASR_BUSY=-7
} lingasr_status_t;
typedef struct {
    const char *model_partition;
    const char *frontend_partition;
    const char *tokens_partition;
    const char *decoder_partition;
} lingasr_config_t;
typedef struct {
    const int16_t *pcm;
    size_t samples;
    unsigned window_ms; /* 0 selects a 4..8 second window; otherwise 4000..8000 in steps of 1000. */
    lingasr_cancel_fn cancelled;
    void *userdata;
} lingasr_request_t;
typedef struct {
    unsigned window_ms;
    size_t text_bytes;
    int64_t frontend_us,inference_us,decode_us,total_us;
} lingasr_result_t;
typedef struct {
    size_t activation_bytes,workspace_bytes,decoder_bytes,pcm_bytes;
} lingasr_memory_t;
const char *lingasr_version(void);
void lingasr_default_config(lingasr_config_t *config);
/* One engine per process. Requires ESP32-S3 + octal PSRAM; resources are hash-verified. */
lingasr_status_t lingasr_create(const lingasr_config_t *config,lingasr_engine_t **engine);
/* Blocking batch inference; call from a >=20KiB task stack. PCM is copied before padding.
 * Concurrent inference returns BUSY. Cancel callback must be short and nonblocking.
 * A full 1024-byte text buffer is required, avoiding silent truncation of final output. */
lingasr_status_t lingasr_transcribe(lingasr_engine_t *engine,const lingasr_request_t *request,
                                  char *text,size_t capacity,lingasr_result_t *result);
lingasr_status_t lingasr_get_memory(const lingasr_engine_t *engine,lingasr_memory_t *memory);
/* Lifecycle operations must be serialized by the application. BUSY keeps the engine alive. */
lingasr_status_t lingasr_destroy(lingasr_engine_t *engine);
#ifdef __cplusplus
}
#endif
