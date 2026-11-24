#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

enum { ASSISTANT_TEXT=1024, ASSISTANT_REPLY=768, ASSISTANT_HISTORY=8,
       ASSISTANT_RESPONSE=32768, ASSISTANT_SUMMARY=2048, ASSISTANT_MEMORY=2048 };
typedef struct {
    uint32_t version;
    char ssid[33],password[65],model[129],base_url[385],api_key[513];
} assistant_config_t;
typedef struct { char user[ASSISTANT_TEXT],reply[ASSISTANT_REPLY]; } assistant_pair_t;
typedef struct {
    unsigned count;
    assistant_pair_t pairs[ASSISTANT_HISTORY];
    char summary[ASSISTANT_SUMMARY],memory[ASSISTANT_MEMORY];
} assistant_history_t;
/* Kept separate from the existing version-1 Wi-Fi/key blob. */
typedef enum { SEARCH_AUTO, SEARCH_OFF, SEARCH_OPENAI, SEARCH_DEEPSEEK } assistant_search_t;
typedef struct {
    uint32_t version;
    int search_provider,rgb_gpio;
    unsigned completion_tokens;
    char search_model[129];
} assistant_options_t;
void assistant_options_default(assistant_options_t *options);
bool assistant_options_parse(const cJSON *json,const assistant_options_t *previous,assistant_options_t *out);
cJSON *assistant_options_public(const assistant_options_t *options);
assistant_search_t assistant_search_provider(const assistant_config_t *config,const assistant_options_t *options);
const char *assistant_search_model(const assistant_options_t *options,assistant_search_t provider);
bool assistant_message_text(const cJSON *message,char *out,size_t capacity,bool shorten);
cJSON *assistant_response_message(const cJSON *root);
bool assistant_message_add(cJSON *messages,const char *role,const char *text);

extern const char assistant_system_prompt[];
bool assistant_config_parse(const char *json,const assistant_config_t *previous,
                            assistant_config_t *output,char *error,size_t capacity);
bool assistant_wifi_config_parse(const char *json,const assistant_config_t *previous,
                            assistant_config_t *output,char *error,size_t capacity);
bool assistant_model_config_parse(const char *json,const assistant_config_t *previous,
                            assistant_config_t *output,char *error,size_t capacity);
bool assistant_config_valid(const assistant_config_t *config);
bool assistant_config_llm_valid(const assistant_config_t *config);
cJSON *assistant_config_public(const assistant_config_t *config);
bool assistant_endpoint(const char *base,char *url,size_t capacity);
char *assistant_chat_request(const assistant_config_t *config,const assistant_history_t *history,const char *user);
bool assistant_chat_response(const char *body,char *reply,size_t capacity,char *error,size_t error_capacity);
void assistant_history_add(assistant_history_t *history,const char *user,const char *reply);
void assistant_copy_text(char *out,size_t capacity,const char *text);
cJSON *assistant_json_parse(const char *text);

/* mode(off/solid/blink/cycle), R, G, B, period_ms, duration_ms */
bool assistant_rgb_arguments(const cJSON *args,unsigned values[6]);
