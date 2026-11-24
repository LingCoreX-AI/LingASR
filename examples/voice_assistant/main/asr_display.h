#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "asr_screen.h"

typedef void (*asr_display_snapshot_fn)(asr_screen_state_t *snapshot);
esp_err_t asr_display_init(asr_display_snapshot_fn snapshot);
esp_err_t asr_display_init_event_driven(asr_display_snapshot_fn snapshot);
void asr_display_notify(void);
bool asr_display_ready(void);
