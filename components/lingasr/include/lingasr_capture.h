/* Copyright (c) 2026 LingCoreX. Public integration interface: MIT. */
#pragma once
#include "lingasr.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct lingasr_capture lingasr_capture_t;
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
void lingasr_capture_destroy(lingasr_capture_t *capture);
void lingasr_capture_observe(lingasr_capture_t *capture,const int16_t *frame);
void lingasr_capture_cancel(lingasr_capture_t *capture);
int lingasr_capture_push(lingasr_capture_t *capture,const int16_t *frame,int16_t *output);
void lingasr_capture_advance(lingasr_capture_t *capture,int16_t *output);
lingasr_status_t lingasr_capture_get_info(const lingasr_capture_t *capture,lingasr_capture_info_t *info);
typedef struct {unsigned ticks;bool stable_down,ready;} lingasr_button_t;
bool lingasr_button_poll(lingasr_button_t *button,bool down);
#ifdef __cplusplus
}
#endif
