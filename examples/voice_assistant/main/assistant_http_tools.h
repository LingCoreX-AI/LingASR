#pragma once
#include "assistant_protocol.h"
enum { ASSISTANT_HTTP_TOOLS=4, ASSISTANT_TOOL_CONFIG_BYTES=4096, ASSISTANT_TOOL_RESPONSE_BYTES=4096 };
cJSON *assistant_tools_default(const assistant_options_t *options);
cJSON *assistant_tools_normalize(const cJSON *input,const cJSON *previous,char *error,size_t capacity);
const cJSON *assistant_tools_find(const cJSON *config,const char *name);
bool assistant_tools_enabled(const cJSON *config,const char *name);
cJSON *assistant_tools_schemas(const cJSON *config);
bool assistant_tools_arguments(const cJSON *definition,const cJSON *arguments,char *error,size_t capacity);
bool assistant_tools_http_request(const cJSON *definition,const cJSON *arguments,char *url,size_t size,
                                 char **body,char *error,size_t capacity);
