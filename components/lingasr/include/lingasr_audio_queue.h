/* Copyright (c) 2026 LingCoreX. Public integration interface: MIT. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "lingasr_capture.h"

#ifdef __cplusplus
extern "C" {
#endif
enum { LINGASR_AUDIO_SLOTS=4 };
enum { LINGASR_AUDIO_FREE,LINGASR_AUDIO_COPYING,LINGASR_AUDIO_READY,LINGASR_AUDIO_RUNNING };
typedef struct {
    int16_t *pcm;
    unsigned state,sequence,samples,window_ms,utterance;
    bool continuation, more;
    unsigned logmel_frames;
    unsigned segment,preview_start,preview_end;
} lingasr_audio_job_t;
typedef struct { lingasr_audio_job_t slots[LINGASR_AUDIO_SLOTS];unsigned sequence; } lingasr_audio_queue_t;
/* Protect metadata with the caller's mutex. Copy PCM while COPYING, outside
 * the mutex. The worker owns each immutable buffer until release. */
int lingasr_audio_reserve(lingasr_audio_queue_t *queue,unsigned samples,unsigned utterance,bool continuation);
void lingasr_audio_commit(lingasr_audio_queue_t *queue,int slot);
int lingasr_audio_take(lingasr_audio_queue_t *queue);
void lingasr_audio_release(lingasr_audio_queue_t *queue,int slot);
/* Join protection-cut continuations, suppressing a matching UTF-8 boundary
 * prefix up to four Chinese characters. Overflow leaves output untouched. */
int lingasr_audio_join(char *output,size_t capacity,const char *previous,const char *next);

#ifdef __cplusplus
}
#endif
