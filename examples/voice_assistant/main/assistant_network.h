#pragma once
#include "assistant_protocol.h"
#include "assistant_timing.h"
#include "esp_err.h"
typedef cJSON *(*assistant_status_fn)(void);
typedef bool (*assistant_submit_fn)(const char *text);
typedef void (*assistant_enable_fn)(bool enabled);
typedef bool (*assistant_cancel_fn)(void *context);
esp_err_t assistant_network_start(assistant_status_fn status,assistant_submit_fn submit,assistant_enable_fn enable);
bool assistant_network_ready(void);
bool assistant_network_configured(void);
bool assistant_network_config_mode(void);
void assistant_network_enter_config_mode(void);
unsigned assistant_network_revision(void);
bool assistant_network_call(const char *user,assistant_history_t *history,char *reply,size_t capacity,
                            char *error,size_t error_capacity,assistant_cancel_fn cancel,void *context);
bool assistant_network_call_traced(const char *user,assistant_history_t *history,char *reply,size_t capacity,
                            char *error,size_t error_capacity,assistant_cancel_fn cancel,void *context,
                            const assistant_timing_t *timing);

void assistant_network_memory_load(assistant_history_t *history);
