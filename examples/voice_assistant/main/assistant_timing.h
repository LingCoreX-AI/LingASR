#pragma once
#include <stdint.h>
/* One device monotonic clock. last_voice_us is a VAD/sample-position estimate,
 * not an externally measured acoustic endpoint. Zero means unavailable. */
typedef struct {
    unsigned turn_id,segment_id;
    int64_t last_voice_us,endpoint_us,queued_us;
    int64_t asr_begin_us,frontend_end_us,infer_end_us,asr_end_us;
    unsigned samples,window_ms,end_reason;
} assistant_timing_t;
