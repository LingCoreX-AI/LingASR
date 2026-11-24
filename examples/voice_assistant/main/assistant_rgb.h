#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "assistant_rgb_pattern.h"
typedef struct {
    assistant_rgb_command_t command;
    unsigned elapsed_ms;
    bool last_write_ok;
    uint32_t frame_rgb;
} assistant_rgb_state_t;
bool assistant_rgb_snapshot(assistant_rgb_state_t *state);
esp_err_t assistant_rgb_start(int gpio);
bool assistant_rgb_set(const assistant_rgb_command_t *command);
