#include "assistant_network.h"
#include "assistant_agent.h"
#include "assistant_rgb.h"
#include "assistant_wifi.h"
#include "assistant_http_tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG="assistant_net";
static SemaphoreHandle_t mutex;
static TaskHandle_t manager_task;
static assistant_config_t saved;
static assistant_options_t saved_options;
static assistant_wifi_store_t wifi_store;
static cJSON *tool_registry,*tool_test_result;
static cJSON *last_turn;
static cJSON *device_state(void *arg);
static atomic_bool tool_test_busy;
static char tool_test_name[49];
typedef struct {cJSON *registry,*args;char name[49];unsigned revision;} tool_test_job_t;
static void tool_test_worker(void *arg);
static assistant_wifi_ap_t scan_aps[ASSISTANT_WIFI_SCAN_MAX];
static unsigned scan_count,scan_generation;
static char scan_error[96],active_ssid[33];
static atomic_bool scan_requested,scanning,scan_done;
static atomic_uint scan_status;
typedef struct { uint32_t version;assistant_config_t config;assistant_options_t options;assistant_wifi_store_t wifi; } settings_v2_t;
static cJSON *last_sources;
static char last_tool[32];
static bool last_tool_ok;
static unsigned tool_count,compactions;
static bool rgb_ready;
static int active_rgb_gpio;
static char saved_summary[ASSISTANT_SUMMARY],saved_memory[ASSISTANT_MEMORY];
static atomic_bool connected;
static atomic_uint revision,connection_revision;
static atomic_int disconnect_reason;
static atomic_bool config_mode,station_connecting;
static atomic_uint ap_clients;
static char address[20];
static assistant_status_fn status_fn;
static assistant_submit_fn submit_fn;
static assistant_enable_fn enable_fn;
extern const char portal[] asm("_binary_index_html_gz_start");
extern const char portal_end[] asm("_binary_index_html_gz_end");
static void http_probe(void *arg);
static char *receive(httpd_req_t *req);

bool assistant_network_ready(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);
    bool ready=!atomic_load(&config_mode) && atomic_load(&connected) && wifi_store.count && assistant_config_llm_valid(&saved);
    xSemaphoreGive(mutex);return ready;
}
unsigned assistant_network_revision(void) { return atomic_load(&revision); }
bool assistant_network_configured(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);bool valid=wifi_store.count && assistant_config_llm_valid(&saved);xSemaphoreGive(mutex);return valid;
}
bool assistant_network_config_mode(void) { return atomic_load(&config_mode); }
void assistant_network_enter_config_mode(void)
{
    atomic_store(&config_mode,true);atomic_fetch_add(&revision,1);xTaskNotifyGive(manager_task);
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;
    if(base==WIFI_EVENT && id==WIFI_EVENT_SCAN_DONE) {
        atomic_store(&scan_status,((wifi_event_sta_scan_done_t *)data)->status);
        atomic_store(&scan_done,true);if(manager_task)xTaskNotifyGive(manager_task);
    }
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&station_connecting,false);
        atomic_store(&disconnect_reason,((wifi_event_sta_disconnected_t *)data)->reason);
        atomic_store(&connected,false);
        if(manager_task)xTaskNotifyGive(manager_task);
    }
    if(base==WIFI_EVENT && id==WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG,"Configuration hotspot client joined; count=%u",atomic_fetch_add(&ap_clients,1)+1);
    }
    if(base==WIFI_EVENT && id==WIFI_EVENT_AP_STADISCONNECTED) {
        unsigned count=atomic_load(&ap_clients);
        while(count && !atomic_compare_exchange_weak(&ap_clients,&count,count-1)){}
        ESP_LOGI(TAG,"Configuration hotspot client left");
    }
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event=data;
        xSemaphoreTake(mutex,portMAX_DELAY);
        snprintf(address,sizeof(address),IPSTR,IP2STR(&event->ip_info.ip));
        wifi_ap_record_t ap={0};
        if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){memcpy(active_ssid,ap.ssid,32);active_ssid[32]=0;}
        xSemaphoreGive(mutex);
        atomic_store(&connected,true);
        atomic_store(&station_connecting,false);
        atomic_store(&disconnect_reason,0);
        ESP_LOGI(TAG,"Station connected; configuration page http://%s",address);
    }
}
static void collect_scan(void)
{
    uint16_t count=0;
    esp_err_t result=!atomic_load(&scan_status) ? esp_wifi_scan_get_ap_num(&count) : ESP_FAIL;
    xSemaphoreTake(mutex,portMAX_DELAY);scan_count=0;
    if(result==ESP_OK)for(unsigned i=0;i<count;i++) {
        wifi_ap_record_t record={0};result=esp_wifi_scan_get_ap_record(&record);
        if(result!=ESP_OK)break;
        record.ssid[32]=0;if(!record.ssid[0])continue;
        int slot=-1;
        for(unsigned j=0;j<scan_count;j++)if(!strcmp(scan_aps[j].ssid,(char *)record.ssid)){slot=(int)j;break;}
        if(slot>=0 && scan_aps[slot].rssi>=record.rssi)continue;
        if(slot<0 && scan_count<ASSISTANT_WIFI_SCAN_MAX)slot=(int)scan_count++;
        if(slot<0) {
            for(unsigned j=0;j<scan_count;j++)if(assistant_wifi_find(&wifi_store,scan_aps[j].ssid)<0 && (slot<0 || scan_aps[j].rssi<scan_aps[slot].rssi))slot=(int)j;
            if(slot<0 || (assistant_wifi_find(&wifi_store,(char *)record.ssid)<0 && scan_aps[slot].rssi>=record.rssi))continue;
        }
        assistant_wifi_ap_t *ap=&scan_aps[slot];memset(ap,0,sizeof(*ap));
        memcpy(ap->ssid,record.ssid,33);ap->rssi=record.rssi;ap->open=record.authmode==WIFI_AUTH_OPEN;
    }
    if(result!=ESP_OK)scan_count=0;
    unsigned visible=0;for(unsigned i=0;i<scan_count;i++)visible+=assistant_wifi_find(&wifi_store,scan_aps[i].ssid)>=0;
    assistant_copy_text(scan_error,sizeof(scan_error),result==ESP_OK ? "" : "Wi-Fi扫描失败，请重试或手动输入");
    scan_generation++;xSemaphoreGive(mutex);esp_wifi_clear_ap_list();atomic_store(&scanning,false);
    ESP_LOGI(TAG,"Wi-Fi scan: unique=%u saved_visible=%u status=%d",scan_count,visible,result);
}
static void wifi_manager(void *arg)
{
    (void)arg;unsigned applied=UINT32_MAX,tried=0;bool was_config=false,selecting=false,force_preferred=false;
#ifdef CONFIG_MBEDTLS_HAVE_TIME_DATE
    bool clock_started=false;
#endif
    wifi_mode_t mode=WIFI_MODE_APSTA;int64_t last_cycle=0,attempt_started=0,scan_started=0;
    for(;;) {
        int64_t now=esp_timer_get_time();
        if(atomic_exchange(&scan_done,false))collect_scan();
        if(atomic_load(&scanning) && now-scan_started>8000000) {
            esp_wifi_scan_stop();esp_wifi_clear_ap_list();atomic_store(&scanning,false);
            xSemaphoreTake(mutex,portMAX_DELAY);scan_count=0;assistant_copy_text(scan_error,sizeof(scan_error),"Wi-Fi扫描超时，可重新扫描或手动输入");xSemaphoreGive(mutex);
        }
        unsigned current=atomic_load(&connection_revision);
        bool provision=atomic_load(&config_mode);
        if(provision && !was_config) {
                esp_wifi_disconnect();atomic_store(&connected,false);atomic_store(&station_connecting,false);
                selecting=false;was_config=true;
                ESP_LOGI(TAG,"Configuration mode; automatic Wi-Fi connections suspended");
                if(xTaskCreate(http_probe,"http_probe",4096,NULL,1,NULL)!=pdPASS)ESP_LOGW(TAG,"HTTP local probe task unavailable");
        }
        if(!provision && was_config){was_config=false;applied=UINT32_MAX;force_preferred=true;}
        wifi_mode_t desired=provision && !atomic_load(&scan_requested) && !atomic_load(&scanning) ? WIFI_MODE_AP : WIFI_MODE_APSTA;
        if(mode!=desired && esp_wifi_set_mode(desired)==ESP_OK)mode=desired;
        if(!provision && applied!=current && !atomic_load(&scanning)) {
            force_preferred|=applied!=UINT32_MAX;applied=current;tried=0;selecting=true;last_cycle=now;
            atomic_store(&connected,false);esp_wifi_disconnect();
            atomic_store(&station_connecting,false);atomic_store(&scan_requested,true);
        }
        if(!provision && !atomic_load(&connected) && !atomic_load(&station_connecting) && !selecting && !atomic_load(&ap_clients) && now-last_cycle>=30000000) {
            xSemaphoreTake(mutex,portMAX_DELAY);bool any=wifi_store.count!=0;xSemaphoreGive(mutex);
            if(any){selecting=true;tried=0;last_cycle=now;atomic_store(&scan_requested,true);}
        }
        if(atomic_load(&station_connecting) && now-attempt_started>20000000) {
            esp_wifi_disconnect();atomic_store(&station_connecting,false);
        }
        if(atomic_load(&scan_requested) && !atomic_load(&scanning) && !atomic_load(&station_connecting) && mode==WIFI_MODE_APSTA) {
            atomic_store(&scan_requested,false);atomic_store(&scanning,true);scan_started=now;
            wifi_scan_config_t cfg={.show_hidden=true,.scan_type=WIFI_SCAN_TYPE_ACTIVE,.scan_time.active={.min=30,.max=100},.home_chan_dwell_time=30};
            esp_err_t err=esp_wifi_scan_start(&cfg,false);
            if(err!=ESP_OK){atomic_store(&scan_status,1);atomic_store(&scan_done,true);}
            if(!provision && !atomic_load(&connected)){selecting=true;tried=0;last_cycle=now;}
        }
        if(!provision && selecting && !atomic_load(&scanning) && !atomic_load(&scan_requested) && !atomic_load(&station_connecting) && !atomic_load(&connected)) {
            wifi_config_t wifi={0};int index;
            xSemaphoreTake(mutex,portMAX_DELAY);
            index=assistant_wifi_choose(&wifi_store,scan_aps,scan_count,tried);
            int preferred=assistant_wifi_find(&wifi_store,wifi_store.preferred);
            if(force_preferred && preferred>=0 && !(tried&(1u<<preferred)))index=preferred;
            force_preferred=false;
            if(index>=0){memcpy(wifi.sta.ssid,wifi_store.profiles[index].ssid,32);memcpy(wifi.sta.password,wifi_store.profiles[index].password,64);}
            xSemaphoreGive(mutex);
            if(index<0)selecting=false;
            else {
                tried|=1u<<index;wifi.sta.pmf_cfg.capable=true;attempt_started=now;
                atomic_store(&station_connecting,true);
                if(esp_wifi_set_config(WIFI_IF_STA,&wifi)!=ESP_OK || esp_wifi_connect()!=ESP_OK)atomic_store(&station_connecting,false);
                ESP_LOGI(TAG,"Trying saved Wi-Fi profile %d",index+1);
            }
        }
        if(atomic_load(&connected))selecting=false;
#ifdef CONFIG_MBEDTLS_HAVE_TIME_DATE
        if(atomic_load(&connected) && !clock_started) {
            esp_sntp_config_t sntp=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if(esp_netif_sntp_init(&sntp)==ESP_OK)clock_started=true;
        }
#endif
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(250));
    }
}
static esp_err_t json_response(httpd_req_t *req,cJSON *root)
{
    if(!root)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");
    char *body=cJSON_PrintUnformatted(root);cJSON_Delete(root);
    if(!body)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");
    httpd_resp_set_type(req,"application/json; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    esp_err_t r=httpd_resp_sendstr(req,body);free(body);return r;
}
static esp_err_t home(httpd_req_t *req)
{
    ESP_LOGI(TAG,"HTTP GET / received");
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"Content-Encoding","gzip");
    httpd_resp_set_hdr(req,"Vary","Accept-Encoding");
    return httpd_resp_send(req,portal,portal_end-portal);
}
static esp_err_t health(httpd_req_t *req)
{
    httpd_resp_set_type(req,"application/json");return httpd_resp_sendstr(req,"{\"ok\":true}");
}
static cJSON *wifi_public_locked(void)
{
    cJSON *list=cJSON_CreateArray();
    for(unsigned i=0;i<wifi_store.count;i++) {
        const assistant_wifi_profile_t *p=&wifi_store.profiles[i];cJSON *j=cJSON_CreateObject();
        cJSON_AddStringToObject(j,"ssid",p->ssid);cJSON_AddBoolToObject(j,"password_set",p->password[0]!=0);
        cJSON_AddBoolToObject(j,"hidden",p->hidden);cJSON_AddBoolToObject(j,"preferred",!strcmp(wifi_store.preferred,p->ssid));
        cJSON_AddItemToArray(list,j);
    }
    return list;
}
static esp_err_t scan_get(httpd_req_t *req)
{
    cJSON *j=cJSON_CreateObject(),*aps=cJSON_AddArrayToObject(j,"networks");
    xSemaphoreTake(mutex,portMAX_DELAY);
    cJSON_AddStringToObject(j,"error",scan_error);cJSON_AddNumberToObject(j,"generation",scan_generation);
    for(unsigned i=0;i<scan_count;i++) {
        cJSON *ap=cJSON_CreateObject();cJSON_AddStringToObject(ap,"ssid",scan_aps[i].ssid);
        cJSON_AddNumberToObject(ap,"rssi",scan_aps[i].rssi);cJSON_AddBoolToObject(ap,"open",scan_aps[i].open);
        cJSON_AddBoolToObject(ap,"saved",assistant_wifi_find(&wifi_store,scan_aps[i].ssid)>=0);cJSON_AddItemToArray(aps,ap);
    }
    xSemaphoreGive(mutex);cJSON_AddBoolToObject(j,"scanning",atomic_load(&scanning)||atomic_load(&scan_requested));
    return json_response(req,j);
}
static esp_err_t scan_post(httpd_req_t *req)
{
    char *body=receive(req);if(!body)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid request");free(body);
    if(!atomic_load(&scanning))atomic_store(&scan_requested,true);
    xTaskNotifyGive(manager_task);return httpd_resp_sendstr(req,"{\"accepted\":true}");
}
static bool bounded_config(const assistant_config_t *c)
{
    return memchr(c->ssid,0,sizeof(c->ssid)) && memchr(c->password,0,sizeof(c->password)) &&
        memchr(c->model,0,sizeof(c->model)) && memchr(c->base_url,0,sizeof(c->base_url)) && memchr(c->api_key,0,sizeof(c->api_key));
}
static bool stored_config_valid(const assistant_config_t *c)
{
    return bounded_config(c) && c->version==1 && ((!c->model[0] && !c->base_url[0] && !c->api_key[0]) || assistant_config_llm_valid(c));
}
static bool options_valid(const assistant_options_t *o)
{
    return o->version==1 && o->search_provider>=SEARCH_AUTO && o->search_provider<=SEARCH_DEEPSEEK &&
        (o->rgb_gpio==48 || o->rgb_gpio==38) && o->completion_tokens>=256 && o->completion_tokens<=4096 && memchr(o->search_model,0,sizeof(o->search_model));
}
static esp_err_t persist_settings(const assistant_config_t *config,const assistant_options_t *options,const assistant_wifi_store_t *wifi)
{
    settings_v2_t *next=calloc(1,sizeof(*next));if(!next)return ESP_ERR_NO_MEM;
    next->version=2;next->config=*config;next->options=*options;next->wifi=*wifi;
    nvs_handle_t h;esp_err_t err=nvs_open("voice_assistant",NVS_READWRITE,&h);
    if(err==ESP_OK){err=nvs_set_blob(h,"settings_v2",next,sizeof(*next));if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}
    free(next);return err;
}
static esp_err_t wifi_delete(httpd_req_t *req)
{
    char *body=receive(req);cJSON *j=body ? assistant_json_parse(body) : NULL;free(body);
    cJSON *ssid=cJSON_GetObjectItemCaseSensitive(j,"ssid");
    if(!cJSON_IsString(ssid) || !ssid->valuestring[0] || strlen(ssid->valuestring)>32){cJSON_Delete(j);return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid SSID");}
    settings_v2_t *next=calloc(1,sizeof(*next));if(!next){cJSON_Delete(j);return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");}
    xSemaphoreTake(mutex,portMAX_DELAY);next->config=saved;next->options=saved_options;next->wifi=wifi_store;xSemaphoreGive(mutex);
    bool found=assistant_wifi_remove(&next->wifi,ssid->valuestring);cJSON_Delete(j);
    if(!found){free(next);return httpd_resp_send_err(req,HTTPD_404_NOT_FOUND,"SSID not saved");}
    memset(next->config.ssid,0,sizeof(next->config.ssid));memset(next->config.password,0,sizeof(next->config.password));
    int preferred=assistant_wifi_find(&next->wifi,next->wifi.preferred);
    if(preferred>=0){memcpy(next->config.ssid,next->wifi.profiles[preferred].ssid,33);memcpy(next->config.password,next->wifi.profiles[preferred].password,65);}
    esp_err_t err=persist_settings(&next->config,&next->options,&next->wifi);
    if(err==ESP_OK){xSemaphoreTake(mutex,portMAX_DELAY);saved=next->config;wifi_store=next->wifi;atomic_fetch_add(&connection_revision,1);atomic_fetch_add(&revision,1);xSemaphoreGive(mutex);xTaskNotifyGive(manager_task);}
    free(next);if(err!=ESP_OK)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot save Wi-Fi profiles");
    return httpd_resp_sendstr(req,"{\"deleted\":true}");
}
static esp_err_t provision(httpd_req_t *req)
{
    char *body=receive(req);
    if(!body)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid request");
    free(body);
    assistant_network_enter_config_mode();enable_fn(false);
    return httpd_resp_sendstr(req,"{}");
}
static cJSON *heap_status(unsigned caps)
{
    cJSON *j=cJSON_CreateObject();if(!j)return NULL;
    cJSON_AddNumberToObject(j,"total_bytes",heap_caps_get_total_size(caps));
    cJSON_AddNumberToObject(j,"free_bytes",heap_caps_get_free_size(caps));
    cJSON_AddNumberToObject(j,"minimum_free_bytes",heap_caps_get_minimum_free_size(caps));
    cJSON_AddNumberToObject(j,"largest_free_block_bytes",heap_caps_get_largest_free_block(caps));
    return j;
}
static esp_err_t status(httpd_req_t *req)
{
    xSemaphoreTake(mutex,portMAX_DELAY);
    cJSON *j=assistant_config_public(&saved);
    if(j)cJSON_AddStringToObject(j,"station_ip",atomic_load(&connected) ? address : "");
    if(j){cJSON_ReplaceItemInObject(j,"configured",cJSON_CreateBool(wifi_store.count && assistant_config_llm_valid(&saved)));
        cJSON_AddItemToObject(j,"wifi_saved",wifi_public_locked());cJSON_AddNumberToObject(j,"wifi_max",ASSISTANT_WIFI_MAX);
        cJSON_AddStringToObject(j,"wifi_preferred",wifi_store.preferred);cJSON_AddStringToObject(j,"current_ssid",atomic_load(&connected) ? active_ssid : "");}
    xSemaphoreGive(mutex);
    if(j) {
        xSemaphoreTake(mutex,portMAX_DELAY);
        cJSON_AddItemToObject(j,"tools",assistant_options_public(&saved_options));
        cJSON_AddNumberToObject(j,"active_rgb_gpio",active_rgb_gpio);
        cJSON_AddBoolToObject(j,"rgb_ready",rgb_ready);
        cJSON_AddStringToObject(j,"last_tool",last_tool);cJSON_AddBoolToObject(j,"last_tool_ok",last_tool_ok);
        cJSON_AddNumberToObject(j,"tool_count",tool_count);cJSON_AddNumberToObject(j,"compactions",compactions);
        if(last_turn)cJSON_AddItemToObject(j,"last_turn",cJSON_Duplicate(last_turn,true));
        cJSON_AddItemToObject(j,"device_state",device_state(NULL));
        cJSON_AddStringToObject(j,"summary",saved_summary);cJSON_AddStringToObject(j,"memory",saved_memory);
        cJSON_AddItemToObject(j,"sources",last_sources ? cJSON_Duplicate(last_sources,true) : cJSON_CreateArray());
        xSemaphoreGive(mutex);
        cJSON_AddBoolToObject(j,"wifi_connected",atomic_load(&connected));
        cJSON_AddBoolToObject(j,"clock_ready",time(NULL)>1700000000);
#ifdef CONFIG_MBEDTLS_HAVE_TIME_DATE
        cJSON_AddBoolToObject(j,"clock_required",true);
#else
        cJSON_AddBoolToObject(j,"clock_required",false);
#endif
        cJSON_AddBoolToObject(j,"wifi_connection_failed",!atomic_load(&config_mode) && !atomic_load(&connected) && !atomic_load(&station_connecting) && !atomic_load(&scanning) && !atomic_load(&scan_requested) && atomic_load(&disconnect_reason)!=0);
        cJSON_AddBoolToObject(j,"config_mode",atomic_load(&config_mode));
        cJSON_AddStringToObject(j,"hotspot",CONFIG_ASSISTANT_AP_SSID);
        cJSON_AddItemToObject(j,"assistant",status_fn());
        cJSON *heap=cJSON_AddObjectToObject(j,"heap");
        if(heap){
            cJSON_AddItemToObject(heap,"internal",heap_status(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
            cJSON_AddItemToObject(heap,"psram",heap_status(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        }
    }
    return json_response(req,j);
}
static char *receive(httpd_req_t *req)
{
    char header[8];
    if(httpd_req_get_hdr_value_str(req,"X-Ling-Config",header,sizeof(header))!=ESP_OK || strcmp(header,"1") ||
       !req->content_len || req->content_len>4096)return NULL;
    char *body=malloc(req->content_len+1);
    if(!body)return NULL;
    size_t used=0;
    while(used<req->content_len) {
        int n=httpd_req_recv(req,body+used,req->content_len-used);
        if(n<=0){free(body);return NULL;}
        used+=(size_t)n;
    }
    if(memchr(body,0,used)){free(body);return NULL;}
    body[used]=0;return body;
}
static esp_err_t configure(httpd_req_t *req)
{
    bool wifi_only=!strcmp(req->uri,"/api/wifi/save"),model_only=!strcmp(req->uri,"/api/model");
    char *body=receive(req),error[256]={0};
    settings_v2_t *next=calloc(1,sizeof(*next));
    if(!next){free(body);return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");}
    assistant_config_t previous;
    xSemaphoreTake(mutex,portMAX_DELAY);previous=saved;xSemaphoreGive(mutex);
    xSemaphoreTake(mutex,portMAX_DELAY);next->options=saved_options;next->wifi=wifi_store;xSemaphoreGive(mutex);
    cJSON *json=body ? assistant_json_parse(body) : NULL;
    cJSON *ssid=cJSON_GetObjectItemCaseSensitive(json,"ssid");
    int known=cJSON_IsString(ssid) ? assistant_wifi_find(&next->wifi,ssid->valuestring) : -1;
    if(known>=0){memcpy(previous.ssid,next->wifi.profiles[known].ssid,33);memcpy(previous.password,next->wifi.profiles[known].password,65);}
    bool hidden=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"hidden_wifi"));
    bool ok=body && (wifi_only?assistant_wifi_config_parse(body,&previous,&next->config,error,sizeof(error)):
        model_only?assistant_model_config_parse(body,&previous,&next->config,error,sizeof(error)):
        assistant_config_parse(body,&previous,&next->config,error,sizeof(error)));
    if(ok && !wifi_only)ok=assistant_options_parse(json,&next->options,&next->options);
    if(ok && !model_only && !assistant_wifi_save(&next->wifi,next->config.ssid,next->config.password,hidden)) {
        ok=false;assistant_copy_text(error,sizeof(error),"最多保存8个Wi-Fi，请先删除一个旧网络");
    }
    cJSON_Delete(json);free(body);
    if(!ok) {
        httpd_resp_set_status(req,"400 Bad Request");
        cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error",error[0] ? error : "无效的配置请求");
        free(next);return json_response(req,j);
    }
    esp_err_t r=persist_settings(&next->config,&next->options,&next->wifi);
    if(r!=ESP_OK){free(next);return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot save configuration");}
    xSemaphoreTake(mutex,portMAX_DELAY);saved=next->config;saved_options=next->options;wifi_store=next->wifi;
    if(!model_only){atomic_fetch_add(&connection_revision,1);atomic_store(&config_mode,false);atomic_store(&connected,false);}
    atomic_fetch_add(&revision,1);xSemaphoreGive(mutex);free(next);
    enable_fn(true);xTaskNotifyGive(manager_task);
    cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"saved",true);return json_response(req,j);
}
static esp_err_t tools_get(httpd_req_t *req)
{
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON *j=cJSON_Duplicate(tool_registry,true);
    if(j){cJSON *test=cJSON_AddObjectToObject(j,"test");cJSON_AddBoolToObject(test,"busy",atomic_load(&tool_test_busy));
        cJSON_AddStringToObject(test,"name",tool_test_name);if(tool_test_result)cJSON_AddItemToObject(test,"result",cJSON_Duplicate(tool_test_result,true));}
    xSemaphoreGive(mutex);return json_response(req,j);
}
static esp_err_t tools_save(httpd_req_t *req)
{
    char *body=receive(req),error[256]={0};cJSON *input=body?assistant_json_parse(body):NULL;free(body);
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON *previous=cJSON_Duplicate(tool_registry,true);assistant_options_t options=saved_options;xSemaphoreGive(mutex);
    cJSON *next=assistant_tools_normalize(input,previous,error,sizeof(error));cJSON_Delete(input);cJSON_Delete(previous);
    if(!next || !assistant_options_parse(cJSON_GetObjectItemCaseSensitive(next,"settings"),&options,&options)){
        cJSON_Delete(next);httpd_resp_set_status(req,"400 Bad Request");cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error",error[0]?error:"工具设置无效");return json_response(req,j);
    }
    char *encoded=cJSON_PrintUnformatted(next);nvs_handle_t h;esp_err_t err=encoded?nvs_open("voice_assistant",NVS_READWRITE,&h):ESP_ERR_NO_MEM;
    if(err==ESP_OK){err=nvs_set_str(h,"tools_v1",encoded);if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}free(encoded);
    if(err!=ESP_OK){cJSON_Delete(next);return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot save tools");}
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON *old=tool_registry;tool_registry=next;saved_options=options;atomic_fetch_add(&revision,1);xSemaphoreGive(mutex);cJSON_Delete(old);
    return httpd_resp_sendstr(req,"{\"saved\":true}");
}
static esp_err_t tools_test(httpd_req_t *req)
{
    char *body=receive(req),error[256]={0};cJSON *j=body?assistant_json_parse(body):NULL;free(body);
    cJSON *name=cJSON_GetObjectItemCaseSensitive(j,"name"),*args=cJSON_GetObjectItemCaseSensitive(j,"arguments");
    tool_test_job_t *job=calloc(1,sizeof(*job));if(!job){cJSON_Delete(j);return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");}
    xSemaphoreTake(mutex,portMAX_DELAY);job->registry=cJSON_Duplicate(tool_registry,true);job->revision=atomic_load(&revision);xSemaphoreGive(mutex);
    const cJSON *definition=cJSON_IsString(name)?assistant_tools_find(job->registry,name->valuestring):NULL;
    bool ok=definition && assistant_tools_enabled(job->registry,name->valuestring) && cJSON_IsObject(args) && assistant_tools_arguments(definition,args,error,sizeof(error));
    if(!ok){cJSON_Delete(j);cJSON_Delete(job->registry);free(job);httpd_resp_set_status(req,"400 Bad Request");cJSON *out=cJSON_CreateObject();cJSON_AddStringToObject(out,"error",error[0]?error:"请选择已保存且启用的HTTP工具");return json_response(req,out);}
    assistant_copy_text(job->name,sizeof(job->name),name->valuestring);job->args=cJSON_Duplicate(args,true);cJSON_Delete(j);
    if(atomic_exchange(&tool_test_busy,true)){cJSON_Delete(job->registry);cJSON_Delete(job->args);free(job);httpd_resp_set_status(req,"409 Conflict");return httpd_resp_sendstr(req,"{\"error\":\"已有工具测试正在运行\"}");}
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON_Delete(tool_test_result);tool_test_result=NULL;assistant_copy_text(tool_test_name,sizeof(tool_test_name),job->name);xSemaphoreGive(mutex);
    if(xTaskCreate(tool_test_worker,"tool_test",8192,job,3,NULL)!=pdPASS){cJSON_Delete(job->registry);cJSON_Delete(job->args);free(job);atomic_store(&tool_test_busy,false);return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot start tool test");}
    httpd_resp_set_status(req,"202 Accepted");return httpd_resp_sendstr(req,"{\"accepted\":true}");
}
static esp_err_t control(httpd_req_t *req)
{
    char *body=receive(req);cJSON *j=body ? assistant_json_parse(body) : NULL;free(body);
    cJSON *enabled=cJSON_GetObjectItemCaseSensitive(j,"enabled");
    if(!cJSON_IsBool(enabled)){cJSON_Delete(j);return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Missing enabled");}
    enable_fn(cJSON_IsTrue(enabled));cJSON_Delete(j);
    j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",true);return json_response(req,j);
}
static esp_err_t text_test(httpd_req_t *req)
{
    char *body=receive(req);cJSON *j=body ? assistant_json_parse(body) : NULL;free(body);
    cJSON *text=cJSON_GetObjectItemCaseSensitive(j,"text");
    bool valid=cJSON_IsString(text) && text->valuestring[0] && strlen(text->valuestring)<ASSISTANT_TEXT;
    bool accepted=valid && assistant_network_ready() && submit_fn(text->valuestring);cJSON_Delete(j);
    if(!accepted){httpd_resp_set_status(req,"409 Conflict");j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error","请先完成联网配置，等待当前对话结束");}
    else {httpd_resp_set_status(req,"202 Accepted");j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"accepted",true);}
    return json_response(req,j);
}
static esp_err_t clear_memory(httpd_req_t *req)
{
    char *body=receive(req);if(!body)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid request");free(body);
    xSemaphoreTake(mutex,portMAX_DELAY);
    nvs_handle_t h;esp_err_t r=nvs_open("voice_assistant",NVS_READWRITE,&h);
    if(r==ESP_OK){r=nvs_set_str(h,"memory","{\"summary\":\"\",\"memory\":\"\"}");if(r==ESP_OK)r=nvs_commit(h);nvs_close(h);}
    if(r==ESP_OK){saved_summary[0]=saved_memory[0]=0;atomic_fetch_add(&revision,1);cJSON_Delete(last_sources);last_sources=NULL;}
    xSemaphoreGive(mutex);
    if(r!=ESP_OK)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot clear memory");
    return httpd_resp_sendstr(req,"{}");
}
void assistant_network_memory_load(assistant_history_t *history)
{
    memset(history,0,sizeof(*history));xSemaphoreTake(mutex,portMAX_DELAY);
    assistant_copy_text(history->summary,sizeof(history->summary),saved_summary);
    assistant_copy_text(history->memory,sizeof(history->memory),saved_memory);xSemaphoreGive(mutex);
}
static void http_probe(void *arg)
{
    (void)arg;vTaskDelay(pdMS_TO_TICKS(1500));
    const char *paths[]={"/health","/","/api/status"};
    for(unsigned i=0;i<3;i++) {
        int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);int status=0;size_t bytes=0;
        if(fd<0){ESP_LOGE(TAG,"HTTP local probe socket failed");continue;}
        struct timeval timeout={.tv_sec=2};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(80)};
        address.sin_addr.s_addr=inet_addr("127.0.0.1");
        if(connect(fd,(struct sockaddr *)&address,sizeof(address))==0) {
            char request[128],buffer[512];
            int length=snprintf(request,sizeof(request),"GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",paths[i]);
            if(send(fd,request,length,0)==length) {
                int n;
                while((n=recv(fd,buffer,sizeof(buffer)-1,0))>0) {
                    buffer[n]=0;if(!bytes)sscanf(buffer,"HTTP/%*s %d",&status);bytes+=(size_t)n;
                    if(bytes>32768)break;
                }
            }
        }
        close(fd);ESP_LOGI(TAG,"HTTP local probe %s: status=%d bytes=%u",paths[i],status,(unsigned)bytes);
    }
    vTaskDelete(NULL);
}
esp_err_t assistant_network_start(assistant_status_fn status_cb,assistant_submit_fn submit,assistant_enable_fn enable)
{
    status_fn=status_cb;submit_fn=submit;enable_fn=enable;
    mutex=xSemaphoreCreateMutex();if(!mutex)return ESP_ERR_NO_MEM;
    esp_err_t r=nvs_flash_init();
    if(r!=ESP_OK)return r; /* Do not erase unrelated namespaces on an error. */
    assistant_options_default(&saved_options);
    assistant_wifi_init(&wifi_store);bool migrated=false,have_snapshot=false;
    nvs_handle_t h;
    if(nvs_open("voice_assistant",NVS_READONLY,&h)==ESP_OK) {
        size_t size=sizeof(saved);
        if(nvs_get_blob(h,"config",&saved,&size)!=ESP_OK || size!=sizeof(saved) || !bounded_config(&saved) || !assistant_config_valid(&saved))memset(&saved,0,sizeof(saved));
        assistant_options_t options;size=sizeof(options);
        if(nvs_get_blob(h,"tool_options",&options,&size)==ESP_OK && size==sizeof(options) && options.version==1 &&
           options.search_provider>=SEARCH_AUTO && options.search_provider<=SEARCH_DEEPSEEK &&
           (options.rgb_gpio==48 || options.rgb_gpio==38) && options.completion_tokens>=256 && options.completion_tokens<=4096 &&
           memchr(options.search_model,0,sizeof(options.search_model)))saved_options=options;
        settings_v2_t *snapshot=calloc(1,sizeof(*snapshot));size=0;
        have_snapshot=nvs_get_blob(h,"settings_v2",NULL,&size)==ESP_OK;
        if(snapshot && have_snapshot) {
            if(size==sizeof(*snapshot) && nvs_get_blob(h,"settings_v2",snapshot,&size)==ESP_OK && snapshot->version==2 && stored_config_valid(&snapshot->config) && options_valid(&snapshot->options) && assistant_wifi_valid(&snapshot->wifi)) {
                saved=snapshot->config;saved_options=snapshot->options;wifi_store=snapshot->wifi;migrated=true;
            } else ESP_LOGW(TAG,"Stored settings snapshot invalid; using legacy configuration");
        }
        free(snapshot);
        size=0;
        if(nvs_get_str(h,"memory",NULL,&size)==ESP_OK && size && size<=16384) {
            char *body=malloc(size);
            if(body && nvs_get_str(h,"memory",body,&size)==ESP_OK) {
                cJSON *j=assistant_json_parse(body),*summary=cJSON_GetObjectItemCaseSensitive(j,"summary"),*memory=cJSON_GetObjectItemCaseSensitive(j,"memory");
                if(cJSON_IsString(summary) && strlen(summary->valuestring)<sizeof(saved_summary))assistant_copy_text(saved_summary,sizeof(saved_summary),summary->valuestring);
                if(cJSON_IsString(memory) && strlen(memory->valuestring)<sizeof(saved_memory))assistant_copy_text(saved_memory,sizeof(saved_memory),memory->valuestring);
                cJSON_Delete(j);
            }
            free(body);
        }
        size=0;
        if(nvs_get_str(h,"tools_v1",NULL,&size)==ESP_OK && size && size<=ASSISTANT_TOOL_CONFIG_BYTES){
            char *encoded=malloc(size);char error[256]={0};
            if(encoded && nvs_get_str(h,"tools_v1",encoded,&size)==ESP_OK){cJSON *input=assistant_json_parse(encoded);tool_registry=assistant_tools_normalize(input,NULL,error,sizeof(error));cJSON_Delete(input);}free(encoded);
        }
        nvs_close(h);
    }
    if(!migrated && assistant_config_valid(&saved)) {
        assistant_wifi_save(&wifi_store,saved.ssid,saved.password,false);
        if(!have_snapshot && persist_settings(&saved,&saved_options,&wifi_store)!=ESP_OK)ESP_LOGW(TAG,"Legacy Wi-Fi retained; migration will retry on save");
    }
    if(!tool_registry)tool_registry=assistant_tools_default(&saved_options);
    if(!tool_registry)return ESP_ERR_NO_MEM;
    assistant_options_parse(cJSON_GetObjectItemCaseSensitive(tool_registry,"settings"),&saved_options,&saved_options);
    active_rgb_gpio=saved_options.rgb_gpio;
    rgb_ready=assistant_rgb_start(active_rgb_gpio)==ESP_OK;
    ESP_LOGI(TAG,"RGB ready=%d gpio=%d",rgb_ready,active_rgb_gpio);
    ESP_LOGI(TAG,"Saved settings loaded: configured=%d api_key_present=%d",assistant_config_valid(&saved),saved.api_key[0]!=0);
    ESP_LOGI(TAG,"Wi-Fi profiles loaded: %u",(unsigned)wifi_store.count);
    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
    if(!esp_netif_create_default_wifi_ap() || !esp_netif_create_default_wifi_sta())return ESP_ERR_NO_MEM;
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    const char *ssid=CONFIG_ASSISTANT_AP_SSID,*pass=CONFIG_ASSISTANT_AP_PASSWORD;
    if(!strlen(ssid) || strlen(ssid)>32 || strlen(pass)<8 || strlen(pass)>63)return ESP_ERR_INVALID_ARG;
    wifi_config_t ap={0};memcpy(ap.ap.ssid,ssid,strlen(ssid));memcpy(ap.ap.password,pass,strlen(pass));
    ap.ap.ssid_len=strlen(ssid);ap.ap.channel=6;ap.ap.max_connection=3;ap.ap.authmode=WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    if(xTaskCreatePinnedToCore(wifi_manager,"assistant_wifi",4096,NULL,3,&manager_task,0)!=pdPASS)return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG,"After Wi-Fi: internal free=%u largest=%u",(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();config.stack_size=8192;config.lru_purge_enable=true;config.max_uri_handlers=16;
    config.server_port=80;config.max_open_sockets=4;
    httpd_handle_t server;ESP_ERROR_CHECK(httpd_start(&server,&config));
    httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=home},{.uri="/api/status",.method=HTTP_GET,.handler=status},
        {.uri="/api/config",.method=HTTP_POST,.handler=configure},{.uri="/api/control",.method=HTTP_POST,.handler=control},
        {.uri="/api/test",.method=HTTP_POST,.handler=text_test},
        {.uri="/api/memory/clear",.method=HTTP_POST,.handler=clear_memory},
        {.uri="/health",.method=HTTP_GET,.handler=health},{.uri="/api/provision",.method=HTTP_POST,.handler=provision},
        {.uri="/api/wifi/scan",.method=HTTP_GET,.handler=scan_get},{.uri="/api/wifi/scan",.method=HTTP_POST,.handler=scan_post},
        {.uri="/api/wifi/delete",.method=HTTP_POST,.handler=wifi_delete},
        {.uri="/api/wifi/save",.method=HTTP_POST,.handler=configure},{.uri="/api/model",.method=HTTP_POST,.handler=configure},
        {.uri="/api/tools",.method=HTTP_GET,.handler=tools_get},{.uri="/api/tools",.method=HTTP_POST,.handler=tools_save},
        {.uri="/api/tools/test",.method=HTTP_POST,.handler=tools_test}};
    for(unsigned i=0;i<sizeof(routes)/sizeof(*routes);i++)ESP_ERROR_CHECK(httpd_register_uri_handler(server,&routes[i]));
    ESP_LOGI(TAG,"Configuration hotspot %s, HTTP listening on port 80, page http://192.168.4.1",ssid);
    if(xTaskCreate(http_probe,"http_probe",4096,NULL,1,NULL)!=pdPASS)ESP_LOGW(TAG,"HTTP local probe task unavailable");
    return ESP_OK;
}
typedef struct { assistant_cancel_fn cancel;void *user; const assistant_timing_t *timing;unsigned request_id;const char *purpose;const cJSON *tool_config;int http_status; } call_context_t;
static bool call_cancel(void *arg){call_context_t *c=arg;return c->cancel && c->cancel(c->user);}
static void request_started(const char *purpose,void *arg)
{
    call_context_t *c=arg;c->request_id++;c->purpose=purpose;
}
static bool http_completion(const assistant_config_t *cfg,const char *body,char **output,
                            char *error,size_t error_capacity,void *arg)
{
    call_context_t *context=arg;
    int64_t started=esp_timer_get_time();*output=NULL;context->http_status=0;
    int64_t opened=0,request_sent=0,first_byte=0;
    if(call_cancel(context))return false;
    if(!assistant_network_ready()){assistant_copy_text(error,error_capacity,"Wi-Fi尚未连接，请检查配置");return false;}
    char url[512];if(!assistant_endpoint(cfg->base_url,url,sizeof(url)))return false;
#ifdef CONFIG_MBEDTLS_HAVE_TIME_DATE
    if(!strncmp(url,"https://",8)) {
        int64_t deadline=esp_timer_get_time()+15000000;
        while(time(NULL)<1700000000 && esp_timer_get_time()<deadline) {
            if(call_cancel(context))return false;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if(time(NULL)<1700000000){assistant_copy_text(error,error_capacity,"网络校时未完成，请稍后再试");return false;}
    }
#endif
    char *response=malloc(ASSISTANT_RESPONSE+1);
    if(!response){assistant_copy_text(error,error_capacity,"网络请求内存不足");return false;}
    esp_http_client_config_t options={.url=url,.timeout_ms=30000,.crt_bundle_attach=esp_crt_bundle_attach,
        .disable_auto_redirect=true,.buffer_size=1024,.buffer_size_tx=1024};
    esp_http_client_handle_t client=esp_http_client_init(&options);
    bool ok=false,complete=false;size_t received=0;int status_code=0;
    if(!client)goto finish;
    esp_http_client_set_method(client,HTTP_METHOD_POST);esp_http_client_set_header(client,"Content-Type","application/json");
    if(cfg->api_key[0]) {
        char authorization[sizeof(cfg->api_key)+8];snprintf(authorization,sizeof(authorization),"Bearer %s",cfg->api_key);
        esp_http_client_set_header(client,"Authorization",authorization);
    }
    int64_t deadline=esp_timer_get_time()+90000000;
    if(call_cancel(context) || esp_http_client_open(client,strlen(body))!=ESP_OK)goto finish;
    opened=esp_timer_get_time();
    size_t sent=0;
    while(sent<strlen(body)) {
        if(call_cancel(context) || esp_timer_get_time()>deadline)goto finish;
        int n=esp_http_client_write(client,body+sent,strlen(body)-sent);if(n<=0)goto finish;sent+=(size_t)n;
    }
    request_sent=esp_timer_get_time();
    ESP_LOGI(TAG,"LATENCY_SENT t=%u s=%u r=%u kind=%s at=%lld endpoint=%lld voice_est=%lld",
        context->timing ? context->timing->turn_id : 0,context->timing ? context->timing->segment_id : 0,
        context->request_id,context->purpose ? context->purpose : "unknown",(long long)request_sent,
        context->timing ? (long long)context->timing->endpoint_us : 0,
        context->timing ? (long long)context->timing->last_voice_us : 0);
    esp_http_client_fetch_headers(client);status_code=esp_http_client_get_status_code(client);
    context->http_status=status_code;
    ESP_LOGI(TAG,"LLM HTTP status=%d",status_code);
    if(status_code!=200)goto finish;
    for(;;) {
        if(call_cancel(context) || esp_timer_get_time()>deadline)goto finish;
        if(received==ASSISTANT_RESPONSE){assistant_copy_text(error,error_capacity,"LLM响应超过32KB，请缩短回复或查询范围");goto finish;}
        int n=esp_http_client_read(client,response+received,ASSISTANT_RESPONSE-received);
        if(n<=0){complete=n==0 && esp_http_client_is_complete_data_received(client);break;}
        if(!first_byte)first_byte=esp_timer_get_time();
        received+=(size_t)n;
    }
    if(!complete)goto finish;
    response[received]=0;
    if(memchr(response,0,received)){assistant_copy_text(error,error_capacity,"LLM响应包含无效零字节");goto finish;}
    /* Diagnostics contain metadata only: never the reply, reasoning or key. */
    cJSON *j=cJSON_Parse(response),*m=assistant_response_message(j);
    cJSON *choice=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j,"choices"),0);
    cJSON *reason=cJSON_GetObjectItemCaseSensitive(choice,"finish_reason"),*content=cJSON_GetObjectItemCaseSensitive(m,"content");
    const char *f=cJSON_IsString(reason) ? reason->valuestring : "missing";
    const char *label=!strcmp(f,"stop") ? "stop" : !strcmp(f,"length") ? "length" : !strcmp(f,"tool_calls") ? "tool_calls" : !strcmp(f,"content_filter") ? "content_filter" : "other";
    ESP_LOGI(TAG,"LLM metadata: finish=%s content_type=%s content_bytes=%u tools=%d",label,
        cJSON_IsString(content) ? "string" : cJSON_IsArray(content) ? "array" : "empty",
        cJSON_IsString(content) ? (unsigned)strlen(content->valuestring) : 0,
        cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(m,"tool_calls")));
    cJSON_Delete(j);ok=!call_cancel(context);
finish:
    ESP_LOGI(TAG,"LATENCY_REQUEST {\"turn_id\":%u,\"segment_id\":%u,\"request_id\":%u,\"purpose\":\"%s\",\"begin_us\":%lld,\"opened_us\":%lld,\"sent_us\":%lld,\"first_body_read_us\":%lld,\"done_us\":%lld,\"endpoint_us\":%lld,\"last_voice_estimate_us\":%lld,\"success\":%d}",
        context->timing ? context->timing->turn_id : 0,context->timing ? context->timing->segment_id : 0,
        context->request_id,context->purpose ? context->purpose : "unknown",(long long)started,(long long)opened,
        (long long)request_sent,(long long)first_byte,(long long)esp_timer_get_time(),
        context->timing ? (long long)context->timing->endpoint_us : 0,
        context->timing ? (long long)context->timing->last_voice_us : 0,ok);
    ESP_LOGI(TAG,"LLM transport finished: success=%d response_bytes=%u elapsed_ms=%.1f",ok,(unsigned)received,(esp_timer_get_time()-started)/1000.0);
    if(client){esp_http_client_close(client);esp_http_client_cleanup(client);}
    if(ok)*output=response;else free(response);
    if(!ok && !error[0]) {
        if(status_code>0 && status_code!=200)snprintf(error,error_capacity,"LLM请求失败（HTTP %d），请检查模型、工具能力和接口配置",status_code);
        else assistant_copy_text(error,error_capacity,status_code==200 ? "LLM响应读取中断或超时" : "LLM连接失败或超时，请检查网络和接口配置");
    }
    return ok;
}
static bool rgb_tool(const cJSON *args,char *error,size_t n,void *arg)
{
    if(call_cancel(arg))return false;
    unsigned v[6];
    if(!assistant_rgb_arguments(args,v)){ESP_LOGW(TAG,"RGB arguments rejected");assistant_copy_text(error,n,"RGB参数无效：颜色0到255；闪烁或轮换周期100到10000毫秒；关灯周期和时长可为0");return false;}
    assistant_rgb_command_t c={.mode=v[0],.red=v[1],.green=v[2],.blue=v[3],.period_ms=v[4],.duration_ms=v[5]};
    bool ok=rgb_ready && assistant_rgb_set(&c);
    ESP_LOGI(TAG,"RGB applied: mode=%u r=%u g=%u b=%u period_ms=%u duration_ms=%u success=%d",v[0],v[1],v[2],v[3],v[4],v[5],ok);
    if(!ok)assistant_copy_text(error,n,"RGB硬件写入失败，请检查引脚和驱动");
    return ok;
}
static cJSON *device_state(void *arg)
{
    (void)arg;cJSON *j=cJSON_CreateObject(),*rgb=cJSON_AddObjectToObject(j,"rgb");assistant_rgb_state_t state={0};
    bool ready=rgb_ready && assistant_rgb_snapshot(&state);cJSON_AddBoolToObject(rgb,"ready",ready);
    if(ready){const assistant_rgb_command_t *c=&state.command;const char *modes[]={"off","solid","blink","cycle","cycle_blink"};
        cJSON_AddStringToObject(rgb,"mode",modes[c->mode]);cJSON_AddNumberToObject(rgb,"red",c->red);cJSON_AddNumberToObject(rgb,"green",c->green);cJSON_AddNumberToObject(rgb,"blue",c->blue);
        cJSON_AddNumberToObject(rgb,"period_ms",c->period_ms);cJSON_AddNumberToObject(rgb,"duration_ms",c->duration_ms);
        cJSON_AddBoolToObject(rgb,"persistent",c->mode!=RGB_OFF && !c->duration_ms);
        cJSON_AddNumberToObject(rgb,"remaining_ms",c->mode!=RGB_OFF && c->duration_ms>state.elapsed_ms?c->duration_ms-state.elapsed_ms:0);
        cJSON_AddBoolToObject(rgb,"last_write_ok",state.last_write_ok);
        cJSON_AddNumberToObject(rgb,"frame_rgb",state.frame_rgb);
    }return j;
}
/* Keep only the latest actual agent turn in RAM. Do not retain headers,
 * API keys, reasoning, arbitrary HTTP arguments or raw provider bodies. */
static bool traced_completion(const assistant_config_t *cfg,const char *body,char **output,
                              char *error,size_t n,void *arg)
{
    call_context_t *context=arg;int64_t begin=esp_timer_get_time();
    bool ok=http_completion(cfg,body,output,error,n,arg);
    cJSON *root=ok?assistant_json_parse(*output):NULL,*message=assistant_response_message(root);
    cJSON *record=cJSON_CreateObject(),*choice=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root,"choices"),0);
    cJSON_AddStringToObject(record,"purpose",context->purpose?context->purpose:"main");
    cJSON_AddBoolToObject(record,"transport_ok",ok);cJSON_AddNumberToObject(record,"elapsed_ms",(esp_timer_get_time()-begin)/1000.0);
    cJSON_AddNumberToObject(record,"http_status",context->http_status);
    if(context->purpose && !strcmp(context->purpose,"route")){
        cJSON *request=assistant_json_parse(body),*tools=cJSON_GetObjectItemCaseSensitive(request,"tools"),*first=cJSON_GetArrayItem(tools,0);
        cJSON *function=cJSON_GetObjectItemCaseSensitive(first,"function"),*name=cJSON_GetObjectItemCaseSensitive(function,"name");
        cJSON_AddNumberToObject(record,"declared_tools",cJSON_GetArraySize(tools));
        if(cJSON_IsString(name) && !strncmp(name->valuestring,"_ling_route",11) && strlen(name->valuestring)<32)cJSON_AddStringToObject(record,"declared_function",name->valuestring);
        cJSON_Delete(request);
    }
    cJSON *reason=cJSON_GetObjectItemCaseSensitive(choice,"finish_reason");
    if(cJSON_IsString(reason) && strlen(reason->valuestring)<32)cJSON_AddStringToObject(record,"finish_reason",reason->valuestring);
    cJSON *calls=cJSON_GetObjectItemCaseSensitive(message,"tool_calls"),*call;
    cJSON *tools=cJSON_AddArrayToObject(record,"tool_calls");
    cJSON_ArrayForEach(call,calls){
        if(cJSON_GetArraySize(tools)>=4)break;
        cJSON *function=cJSON_GetObjectItemCaseSensitive(call,"function"),*name=cJSON_GetObjectItemCaseSensitive(function,"name");
        if(!cJSON_IsString(name))continue;
        cJSON *tool=cJSON_CreateObject();
        const char *label=!strcmp(name->valuestring,"rgb_control")?"rgb_control":!strcmp(name->valuestring,"web_search")?"web_search":!strcmp(name->valuestring,"memory_update")?"memory_update":!strncmp(name->valuestring,"_ling_route",11) && !assistant_tools_find(context->tool_config,name->valuestring)?"route":"other";
        cJSON_AddStringToObject(tool,"name",label);
        if(!strcmp(label,"rgb_control")){
            cJSON *encoded=cJSON_GetObjectItemCaseSensitive(function,"arguments"),*args=cJSON_IsString(encoded)?assistant_json_parse(encoded->valuestring):NULL;
            cJSON *selected=cJSON_AddObjectToObject(tool,"arguments"),*mode=cJSON_GetObjectItemCaseSensitive(args,"mode");
            const char *s=cJSON_IsString(mode)?mode->valuestring:"invalid";
            cJSON_AddStringToObject(selected,"mode",(!strcmp(s,"off") || !strcmp(s,"solid") || !strcmp(s,"blink") || !strcmp(s,"cycle") || !strcmp(s,"cycle_blink"))?s:"invalid");
            const char *fields[]={"red","green","blue","period_ms","duration_ms"};
            for(unsigned i=0;i<5;i++){cJSON *value=cJSON_GetObjectItemCaseSensitive(args,fields[i]);if(cJSON_IsNumber(value))cJSON_AddNumberToObject(selected,fields[i],value->valuedouble);}
            unsigned values[6];cJSON_AddBoolToObject(tool,"arguments_valid",assistant_rgb_arguments(args,values));cJSON_Delete(args);
        }else if(!strcmp(label,"route")){
            cJSON *encoded=cJSON_GetObjectItemCaseSensitive(function,"arguments"),*args=cJSON_IsString(encoded)?assistant_json_parse(encoded->valuestring):NULL;
            cJSON *kind=cJSON_GetObjectItemCaseSensitive(args,"kind"),*target=cJSON_GetObjectItemCaseSensitive(args,"tool");
            if(cJSON_IsString(kind) && (!strcmp(kind->valuestring,"answer") || !strcmp(kind->valuestring,"clarify") || !strcmp(kind->valuestring,"tool")))cJSON_AddStringToObject(tool,"kind",kind->valuestring);
            if(cJSON_IsString(target) && assistant_tools_enabled(context->tool_config,target->valuestring))cJSON_AddStringToObject(tool,"selected_tool",target->valuestring);
            cJSON_Delete(args);
        }
        cJSON_AddItemToArray(tools,tool);
    }
    cJSON_Delete(root);xSemaphoreTake(mutex,portMAX_DELAY);
    cJSON *requests=cJSON_GetObjectItemCaseSensitive(last_turn,"requests");
    if(requests){if(cJSON_GetArraySize(requests)>=8)cJSON_DeleteItemFromArray(requests,0);cJSON_AddItemToArray(requests,record);}else cJSON_Delete(record);
    xSemaphoreGive(mutex);return ok;
}
typedef struct {char *body;size_t used;bool overflow;} tool_response_t;
static esp_err_t tool_http_event(esp_http_client_event_t *event)
{
    tool_response_t *r=event->user_data;
    if(event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0){
        if(r->used+(size_t)event->data_len>=ASSISTANT_TOOL_RESPONSE_BYTES){r->overflow=true;return ESP_FAIL;}
        memcpy(r->body+r->used,event->data,event->data_len);r->used+=event->data_len;r->body[r->used]=0;
    }
    return ESP_OK;
}
static cJSON *http_tool(const char *name,const cJSON *args,char *error,size_t n,void *arg)
{
    call_context_t *context=arg;
    const cJSON *definition=assistant_tools_find(context->tool_config,name);
    if(call_cancel(context) || !definition || !assistant_tools_enabled(context->tool_config,name))return NULL;
    char url[1536],*body=NULL;
    if(!assistant_tools_http_request(definition,args,url,sizeof(url),&body,error,n))return NULL;
    tool_response_t response={.body=calloc(1,ASSISTANT_TOOL_RESPONSE_BYTES)};
    if(!response.body){free(body);assistant_copy_text(error,n,"HTTP工具内存不足");return NULL;}
    esp_http_client_config_t config={.url=url,.timeout_ms=10000,.crt_bundle_attach=esp_crt_bundle_attach,
        .disable_auto_redirect=true,.buffer_size=512,.buffer_size_tx=2048,.event_handler=tool_http_event,.user_data=&response};
    esp_http_client_handle_t client=esp_http_client_init(&config);int status=0;esp_err_t err=ESP_FAIL;
    if(client){
        esp_http_client_set_method(client,body?HTTP_METHOD_POST:HTTP_METHOD_GET);
        if(body){esp_http_client_set_header(client,"Content-Type","application/json");esp_http_client_set_post_field(client,body,strlen(body));}
        /* Tool endpoints never receive the LLM provider's Authorization key. */
        if(!call_cancel(context))err=esp_http_client_perform(client);
        status=esp_http_client_get_status_code(client);esp_http_client_cleanup(client);
    }
    cJSON *result=NULL;
    if(err==ESP_OK && status>=200 && status<300 && !response.overflow && !call_cancel(context) && strlen(response.body)==response.used){
        result=cJSON_CreateObject();cJSON_AddBoolToObject(result,"ok",true);cJSON_AddNumberToObject(result,"http_status",status);
        cJSON *data=response.used?assistant_json_parse(response.body):NULL;
        if(data)cJSON_AddItemToObject(result,"data",data);else cJSON_AddStringToObject(result,"text",response.body);
    }else if(!error[0]){
        if(response.overflow)assistant_copy_text(error,n,"HTTP工具响应过长");
        else if(call_cancel(context))assistant_copy_text(error,n,"配置已更改，工具请求已取消");
        else if(status)snprintf(error,n,"HTTP工具请求失败（HTTP %d）",status);
        else assistant_copy_text(error,n,"HTTP工具连接失败或超时");
    }
    ESP_LOGI(TAG,"HTTP tool response: status=%d bytes=%u success=%d",status,(unsigned)response.used,result!=NULL);
    free(body);free(response.body);return result;
}
static bool tool_test_cancel(void *arg){return atomic_load(&revision)!=((tool_test_job_t *)arg)->revision;}
static void tool_test_worker(void *arg)
{
    tool_test_job_t *job=arg;char error[256]={0};
    call_context_t context={.cancel=tool_test_cancel,.user=job,.tool_config=job->registry};
    cJSON *result=http_tool(job->name,job->args,error,sizeof(error),&context);
    if(!result){result=cJSON_CreateObject();cJSON_AddBoolToObject(result,"ok",false);cJSON_AddStringToObject(result,"error",error[0]?error:"工具请求已取消");}
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON_Delete(tool_test_result);tool_test_result=result;atomic_store(&tool_test_busy,false);xSemaphoreGive(mutex);
    cJSON_Delete(job->registry);cJSON_Delete(job->args);free(job);vTaskDelete(NULL);
}
static bool persist_memory(const assistant_history_t *h,void *arg)
{
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"summary",h->summary);cJSON_AddStringToObject(j,"memory",h->memory);
    char *body=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!body)return false;
    xSemaphoreTake(mutex,portMAX_DELAY);
    bool ok=!call_cancel(arg);nvs_handle_t handle;esp_err_t r=ESP_FAIL;
    if(ok && nvs_open("voice_assistant",NVS_READWRITE,&handle)==ESP_OK) {
        r=nvs_set_str(handle,"memory",body);if(r==ESP_OK)r=nvs_commit(handle);nvs_close(handle);
    }
    ok=ok && r==ESP_OK;
    if(ok){assistant_copy_text(saved_summary,sizeof(saved_summary),h->summary);assistant_copy_text(saved_memory,sizeof(saved_memory),h->memory);}
    xSemaphoreGive(mutex);free(body);return ok;
}
static void tool_event(const char *name,bool ok,void *arg)
{
    (void)arg;xSemaphoreTake(mutex,portMAX_DELAY);assistant_copy_text(last_tool,sizeof(last_tool),name);last_tool_ok=ok;
    if(!strcmp(name,"compact")){if(ok)compactions++;}else tool_count++;
    if(last_turn){cJSON_DeleteItemFromObjectCaseSensitive(last_turn,"last_tool");cJSON_DeleteItemFromObjectCaseSensitive(last_turn,"last_tool_ok");cJSON_AddStringToObject(last_turn,"last_tool",name);cJSON_AddBoolToObject(last_turn,"last_tool_ok",ok);}
    xSemaphoreGive(mutex);ESP_LOGI(TAG,"Agent event: %s success=%d",name,ok);
}
static void decision_selected(const char *kind,const char *tool,bool explicit_function,void *arg)
{
    (void)arg;xSemaphoreTake(mutex,portMAX_DELAY);
    if(last_turn){cJSON *decision=cJSON_AddObjectToObject(last_turn,"decision");cJSON_AddStringToObject(decision,"kind",kind);cJSON_AddStringToObject(decision,"tool",tool);cJSON_AddStringToObject(decision,"source",explicit_function?"explicit_function_selection":"routing_function");}
    xSemaphoreGive(mutex);
}
bool assistant_network_call_traced(const char *user,assistant_history_t *history,char *reply,size_t capacity,
                            char *error,size_t error_capacity,assistant_cancel_fn cancel,void *context,
                            const assistant_timing_t *timing)
{
    assistant_config_t cfg;assistant_options_t options;
    xSemaphoreTake(mutex,portMAX_DELAY);cfg=saved;options=saved_options;cJSON *registry=cJSON_Duplicate(tool_registry,true);cJSON_Delete(last_sources);last_sources=NULL;xSemaphoreGive(mutex);
    if(!registry){assistant_copy_text(error,error_capacity,"工具配置内存不足");return false;}
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON_Delete(last_turn);last_turn=cJSON_CreateObject();
    cJSON_AddStringToObject(last_turn,"input",user);cJSON_AddArrayToObject(last_turn,"requests");cJSON_AddBoolToObject(last_turn,"complete",false);xSemaphoreGive(mutex);
    ESP_LOGI(TAG,"Agent request: input_bytes=%u recent_turns=%u",(unsigned)strlen(user),history->count);
    call_context_t io_context={.cancel=cancel,.user=context,.timing=timing,.tool_config=registry};
    assistant_agent_io_t io={.exchange=traced_completion,.rgb=rgb_tool,.persist=persist_memory,.cancel=call_cancel,.event=tool_event,.request=request_started,.context=&io_context,.tool_config=registry,.http_tool=http_tool,.semantic_routing=true,.device_state=device_state,.decision=decision_selected};
    cJSON *sources=NULL;
    bool ok=assistant_agent_run(&cfg,&options,history,user,reply,capacity,error,error_capacity,&sources,&io);
    xSemaphoreTake(mutex,portMAX_DELAY);if(last_turn){cJSON_ReplaceItemInObjectCaseSensitive(last_turn,"complete",cJSON_CreateBool(true));cJSON_AddBoolToObject(last_turn,"success",ok);cJSON_AddStringToObject(last_turn,"reply",ok?reply:"");cJSON_AddStringToObject(last_turn,"error",error);}xSemaphoreGive(mutex);
    xSemaphoreTake(mutex,portMAX_DELAY);if(!call_cancel(&io_context))last_sources=sources;else cJSON_Delete(sources);xSemaphoreGive(mutex);cJSON_Delete(registry);return ok;
}
bool assistant_network_call(const char *user,assistant_history_t *history,char *reply,size_t capacity,
                            char *error,size_t error_capacity,assistant_cancel_fn cancel,void *context)
{
    return assistant_network_call_traced(user,history,reply,capacity,error,error_capacity,cancel,context,NULL);
}
