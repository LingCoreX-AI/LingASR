/* Copyright (c) 2026 LingCoreX. Public integration interface: MIT. */
#pragma once
#include "lingasr.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct lingasr_capture lingasr_capture_t;
typedef enum { LINGASR_VAD_BUILTIN=0,LINGASR_VAD_EXTERNAL=1 } lingasr_vad_source_t;
/* Initialize with default_config(). All times are multiples of 20ms. */
typedef struct {
    size_t struct_size;
    lingasr_vad_source_t vad_source;
    unsigned vad_mode;           /* WebRTC 0..3; default 2. */
    unsigned end_silence_ms;     /* Default 800; 20..8000. */
    unsigned split_after_ms;     /* Default 4000; 0 disables pause splitting. */
    unsigned split_silence_ms;   /* Default 160; 20..end_silence_ms. */
    unsigned max_segment_ms;    /* Default 8000; 1000..8000. */
    unsigned preroll_ms;        /* Default 300; 0..300. Trigger frame is retained. */
    unsigned min_speech_ms;     /* Default 100; accepted speech needed per segment. */
    unsigned onset_window_ms;   /* Default 120; 20..640. */
    unsigned onset_speech_ms;   /* Default 80; speech votes within onset window. */
    unsigned min_rms;           /* Default 100; 0 disables absolute energy gate. */
    float noise_ratio;          /* Default 4; 0 disables adaptive energy gate. */
} lingasr_capture_config_t;
enum { LINGASR_CAPTURE_FRAME=320, LINGASR_CAPTURE_PREROLL=4800,
       LINGASR_CAPTURE_TARGET=64000,LINGASR_CAPTURE_SAMPLES=128000 };
enum { LINGASR_END_NONE,LINGASR_END_SILENCE,LINGASR_END_PAUSE,LINGASR_END_PROTECTION };
enum { LINGASR_CAPTURE_ERROR=-1,LINGASR_CAPTURE_WAIT,LINGASR_CAPTURE_STARTED,
       LINGASR_CAPTURE_RECORDING,LINGASR_CAPTURE_READY,LINGASR_CAPTURE_DISCARDED };
typedef struct {
    unsigned samples,buffered_samples,last_voice_frame,reason;
    bool continuation;
} lingasr_capture_info_t;
lingasr_status_t lingasr_capture_create(lingasr_capture_t **capture);
void lingasr_capture_default_config(lingasr_capture_config_t *config);
lingasr_status_t lingasr_capture_create_ex(const lingasr_capture_config_t *config,lingasr_capture_t **capture);
/* Idle only. Successful update clears onset/pre-roll and classifier/noise state;
 * invalid config leaves state intact. Serialize capture operations in one task. */
lingasr_status_t lingasr_capture_set_config(lingasr_capture_t *capture,const lingasr_capture_config_t *config);
lingasr_status_t lingasr_capture_get_config(const lingasr_capture_t *capture,lingasr_capture_config_t *config);
void lingasr_capture_destroy(lingasr_capture_t *capture);
void lingasr_capture_observe(lingasr_capture_t *capture,const int16_t *frame);
void lingasr_capture_cancel(lingasr_capture_t *capture);
int lingasr_capture_push(lingasr_capture_t *capture,const int16_t *frame,int16_t *output);
/* EXTERNAL mode only: use caller's decision without internal classification or
 * energy gating. Feed every frame, including non-speech, for endpoint timing. */
int lingasr_capture_push_vad(lingasr_capture_t *capture,const int16_t *frame,bool is_speech,int16_t *output);
void lingasr_capture_advance(lingasr_capture_t *capture,int16_t *output);
lingasr_status_t lingasr_capture_get_info(const lingasr_capture_t *capture,lingasr_capture_info_t *info);
typedef struct {unsigned ticks;bool stable_down,ready;} lingasr_button_t;
bool lingasr_button_poll(lingasr_button_t *button,bool down);
#ifdef __cplusplus
}
#endif
