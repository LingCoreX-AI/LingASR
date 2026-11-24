#include "assistant_rgb.h"
#include <stdbool.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include "esp_timer.h"
#include "esp_log.h"

static rmt_channel_handle_t channel;
static rmt_encoder_handle_t encoder;
static QueueHandle_t commands;
static SemaphoreHandle_t command_lock,applied;
static atomic_bool applied_ok;
static portMUX_TYPE state_lock=portMUX_INITIALIZER_UNLOCKED;
static assistant_rgb_command_t reported;
static int64_t reported_start;
static bool reported_ok;
static uint32_t reported_frame;
static esp_err_t pixel(unsigned r,unsigned g,unsigned b)
{
    /* Limit the brightness to 25 percent. The physical order is GRB. */
    uint32_t grb=((g/4)<<16)|((r/4)<<8)|(b/4);
    rmt_symbol_word_t symbols[25]={0};
    for(unsigned i=0;i<24;i++) {
        bool bit=(grb>>(23-i))&1;
        symbols[i]=(rmt_symbol_word_t){.level0=1,.duration0=bit?9:3,.level1=0,.duration1=bit?3:9};
    }
    /* 300us reset also supports WS2812 revisions requiring >280us. */
    symbols[24]=(rmt_symbol_word_t){.level0=0,.duration0=1500,.level1=0,.duration1=1500};
    rmt_transmit_config_t tx={0};
    esp_err_t result=rmt_transmit(channel,encoder,symbols,sizeof(symbols),&tx);
    if(result==ESP_OK)result=rmt_tx_wait_all_done(channel,100);
    return result;
}
static void worker(void *arg)
{
    (void)arg;assistant_rgb_command_t c={0};int64_t start=0;unsigned previous=~0U;
    for(;;) {
        assistant_rgb_command_t next;
        bool changed=xQueueReceive(commands,&next,pdMS_TO_TICKS(20))==pdTRUE;
        if(changed){c=next;start=esp_timer_get_time();previous=~0U;}
        unsigned elapsed=(unsigned)((esp_timer_get_time()-start)/1000);
        if(c.mode!=RGB_OFF && c.duration_ms && elapsed>=c.duration_ms){c.mode=RGB_OFF;previous=~0U;}
        unsigned r,g,b;unsigned frame=assistant_rgb_frame(&c,elapsed,&r,&g,&b);
        if(frame==previous)continue;
        previous=frame;
        esp_err_t result=pixel(r,g,b);
        portENTER_CRITICAL(&state_lock);reported=c;reported_start=start;reported_ok=result==ESP_OK;if(result==ESP_OK)reported_frame=frame;portEXIT_CRITICAL(&state_lock);
        if(changed){atomic_store(&applied_ok,result==ESP_OK);xSemaphoreGive(applied);}
        if(result!=ESP_OK)ESP_LOGE("assistant_rgb","RMT write failed: %s",esp_err_to_name(result));
    }
}
bool assistant_rgb_snapshot(assistant_rgb_state_t *state)
{
    if(!state || !commands)return false;
    int64_t start;portENTER_CRITICAL(&state_lock);
    state->command=reported;start=reported_start;state->last_write_ok=reported_ok;state->frame_rgb=reported_frame;
    portEXIT_CRITICAL(&state_lock);state->elapsed_ms=(unsigned)((esp_timer_get_time()-start)/1000);return true;
}
esp_err_t assistant_rgb_start(int gpio)
{
    if(commands)return ESP_ERR_INVALID_STATE;
    if(gpio!=48 && gpio!=38)return ESP_ERR_INVALID_ARG;
    rmt_tx_channel_config_t cfg={.gpio_num=gpio,.clk_src=RMT_CLK_SRC_DEFAULT,.resolution_hz=10000000,
        .mem_block_symbols=64,.trans_queue_depth=1};
    esp_err_t r=rmt_new_tx_channel(&cfg,&channel);
    rmt_copy_encoder_config_t enc={};
    if(r==ESP_OK)r=rmt_new_copy_encoder(&enc,&encoder);
    if(r==ESP_OK)r=rmt_enable(channel);
    if(r==ESP_OK)r=pixel(0,0,0);
    if(r==ESP_OK) {
        commands=xQueueCreate(1,sizeof(assistant_rgb_command_t));command_lock=xSemaphoreCreateMutex();applied=xSemaphoreCreateBinary();
        if(!commands || !command_lock || !applied)r=ESP_ERR_NO_MEM;
        else if(xTaskCreate(worker,"assistant_rgb",3072,NULL,2,NULL)!=pdPASS){vQueueDelete(commands);commands=NULL;r=ESP_ERR_NO_MEM;}
    }
    if(r!=ESP_OK) {
        if(commands){vQueueDelete(commands);commands=NULL;}
        if(command_lock){vSemaphoreDelete(command_lock);command_lock=NULL;}
        if(applied){vSemaphoreDelete(applied);applied=NULL;}
        if(channel){rmt_disable(channel);rmt_del_channel(channel);channel=NULL;}
        if(encoder){rmt_del_encoder(encoder);encoder=NULL;}
    }
    return r;
}
bool assistant_rgb_set(const assistant_rgb_command_t *c)
{
    if(!commands || !c || c->mode<RGB_OFF || c->mode>RGB_CYCLE_BLINK || c->duration_ms>60000 ||
       (c->mode>=RGB_BLINK && (c->period_ms<100 || c->period_ms>10000)))return false;
    xSemaphoreTake(command_lock,portMAX_DELAY);
    xSemaphoreTake(applied,0);
    bool ok=xQueueOverwrite(commands,c)==pdPASS;
    /* Worker runs outside ASR/TTS computation; do not return success until
     * the first RMT frame has actually been transmitted. */
    if(ok){xSemaphoreTake(applied,portMAX_DELAY);ok=atomic_load(&applied_ok);}
    xSemaphoreGive(command_lock);return ok;
}
