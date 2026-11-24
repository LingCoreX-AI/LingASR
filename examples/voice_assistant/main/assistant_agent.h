#pragma once
#include "assistant_protocol.h"
typedef struct {
    /* exchange owns neither request nor config; caller frees the returned body. */
    bool (*exchange)(const assistant_config_t *,const char *,char **,char *,size_t,void *);
    bool (*rgb)(const cJSON *,char *,size_t,void *);
    bool (*persist)(const assistant_history_t *,void *);
    bool (*cancel)(void *);
    void (*event)(const char *,bool,void *);
    void (*request)(const char *purpose,void *);
    void *context;
    const cJSON *tool_config;
    cJSON *(*http_tool)(const char *,const cJSON *,char *,size_t,void *);
    bool semantic_routing;
    cJSON *(*device_state)(void *);
    void (*decision)(const char *kind,const char *tool,bool explicit_function,void *);
} assistant_agent_io_t;
bool assistant_agent_run(const assistant_config_t *,const assistant_options_t *,assistant_history_t *,
                         const char *,char *,size_t,char *,size_t,cJSON **,const assistant_agent_io_t *);
