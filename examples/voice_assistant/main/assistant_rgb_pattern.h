#pragma once
#include <stdint.h>
/* One WS2812. A zero duration runs until another command. */
typedef enum { RGB_OFF, RGB_SOLID, RGB_BLINK, RGB_CYCLE, RGB_CYCLE_BLINK } assistant_rgb_mode_t;
typedef struct {
    assistant_rgb_mode_t mode;
    uint8_t red,green,blue;
    unsigned period_ms,duration_ms;
} assistant_rgb_command_t;
uint32_t assistant_rgb_frame(const assistant_rgb_command_t *command,unsigned elapsed_ms,unsigned *red,unsigned *green,unsigned *blue);
