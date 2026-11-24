#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ASR_SCREEN_WIDTH 240
#define ASR_SCREEN_HEIGHT 240
typedef struct {
    unsigned id, progress, recorded_ms;
    bool monitoring, draft;
    unsigned window_ms;
    char state[24], text[512], error[128];
    double inference_ms;
    unsigned draft_head_bytes;
} asr_screen_state_t;

/* Show the committed prefix and, when requested, one additional UTF-8
 * character. Return whether more draft text is waiting to be shown. */
bool asr_screen_draft_step(asr_screen_state_t *state, const asr_screen_state_t *previous, bool advance);
unsigned asr_screen_page_count(const asr_screen_state_t *state);
bool asr_screen_append_rows(const asr_screen_state_t *state,const asr_screen_state_t *previous,
                            unsigned page,unsigned *first,unsigned *stop);
/* Native RGB565 framebuffer; panel transport handles wire byte order. */
unsigned asr_screen_render(uint16_t *frame, const asr_screen_state_t *state, unsigned page);
