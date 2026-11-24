/* Application example: MIT. Core recognition is provided by the binary SDK. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lingtts_engine.h"
#include "lingasr.h"
#include "lingasr_capture.h"
#include "lingasr_audio_queue.h"
#include "asr_display.h"
#include "assistant_network.h"
#ifndef CONFIG_ASSISTANT_ZERO_TAIL
#define CONFIG_ASSISTANT_ZERO_TAIL 0
#endif
#ifndef CONFIG_ASSISTANT_PARALLEL_QUANT
#define CONFIG_ASSISTANT_PARALLEL_QUANT 0
#endif

static const char *TAG="assistant";
static void *psalloc(size_t bytes);
static const char READY[]="ready，\n等待语音指令";
static SemaphoreHandle_t mutex,play_done;
static TaskHandle_t turn_task,play_task;
static i2s_chan_handle_t rx;
static lingasr_engine_t *recognizer;
static int16_t *pcm;
static lingasr_capture_t *capture;
static lingasr_audio_queue_t audio;
static unsigned job_reason[LINGASR_AUDIO_SLOTS],job_epoch[LINGASR_AUDIO_SLOTS],job_revision[LINGASR_AUDIO_SLOTS];
static assistant_timing_t job_timing[LINGASR_AUDIO_SLOTS];
static atomic_uint turn_sequence;
static atomic_uint epoch=1,pending;
static atomic_bool enabled=true,mic_warm,turn_closed,capturing,worker_running,tts_active;
static atomic_bool synth_done,play_failed;
static atomic_uint overflows;
static StreamBufferHandle_t pcm_queue;
static StaticStreamBuffer_t pcm_control;
static lingtts_engine_t *tts;
static assistant_history_t *history;
static struct {
    char phase[24],display[512],user[ASSISTANT_TEXT],reply[ASSISTANT_REPLY],error[256],manual[ASSISTANT_TEXT];
    int64_t resume_after;
    unsigned utterance;
} state;
typedef struct { unsigned epoch,revision;assistant_timing_t timing; } turn_context_t;
static void *psalloc(size_t bytes)
{
    void *p=heap_caps_aligned_alloc(16,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(p ? ESP_OK : ESP_ERR_NO_MEM);memset(p,0,bytes);return p;
}
static void display_text(const char *text)
{
    xSemaphoreTake(mutex,portMAX_DELAY);assistant_copy_text(state.display,sizeof(state.display),text);xSemaphoreGive(mutex);
}
static void phase(const char *value)
{
    xSemaphoreTake(mutex,portMAX_DELAY);assistant_copy_text(state.phase,sizeof(state.phase),value);xSemaphoreGive(mutex);
}
static void screen_snapshot(asr_screen_state_t *s)
{
    memset(s,0,sizeof(*s));xSemaphoreTake(mutex,portMAX_DELAY);
    assistant_copy_text(s->text,sizeof(s->text),state.display);xSemaphoreGive(mutex);
}
static cJSON *status_snapshot(void)
{
    cJSON *j=cJSON_CreateObject();if(!j)return NULL;
    xSemaphoreTake(mutex,portMAX_DELAY);
    cJSON_AddStringToObject(j,"phase",state.phase);cJSON_AddStringToObject(j,"user",state.user);
    cJSON_AddStringToObject(j,"reply",state.reply);cJSON_AddStringToObject(j,"error",state.error);
    xSemaphoreGive(mutex);
    cJSON_AddBoolToObject(j,"enabled",atomic_load(&enabled));
    cJSON_AddBoolToObject(j,"busy",atomic_load(&turn_closed)||atomic_load(&worker_running)||atomic_load(&capturing));
    cJSON_AddBoolToObject(j,"microphone_warm",atomic_load(&mic_warm));return j;
}
static bool cancelled(void *context)
{
    turn_context_t *c=context;
    return !atomic_load(&enabled) || atomic_load(&epoch)!=c->epoch || assistant_network_revision()!=c->revision;
}
static void set_enabled(bool value)
{
    atomic_fetch_add(&epoch,1);atomic_store(&enabled,value);atomic_store(&turn_closed,false);
    xSemaphoreTake(mutex,portMAX_DELAY);state.manual[0]=0;state.error[0]=0;xSemaphoreGive(mutex);
    ESP_LOGI(TAG,"Voice assistant %s",value ? "enabled" : "paused");
}
static bool submit_text(const char *text)
{
    xSemaphoreTake(mutex,portMAX_DELAY);
    bool ok=atomic_load(&enabled) && atomic_load(&mic_warm) && !atomic_load(&turn_closed) &&
        !atomic_load(&worker_running) && !atomic_load(&capturing) && !atomic_load(&pending);
    if(ok){assistant_copy_text(state.manual,sizeof(state.manual),text);atomic_store(&turn_closed,true);}
    xSemaphoreGive(mutex);if(ok)xTaskNotifyGive(turn_task);return ok;
}
static void speaker_quiet(void)
{
    const int pins[]={CONFIG_ASR_SPEAKER_BCLK,CONFIG_ASR_SPEAKER_WS,CONFIG_ASR_SPEAKER_DOUT};
    uint64_t mask=0;
    for(unsigned i=0;i<3;i++){gpio_set_level(pins[i],0);mask|=1ULL<<pins[i];}
    gpio_config_t config={.pin_bit_mask=mask,.mode=GPIO_MODE_OUTPUT,.pull_down_en=GPIO_PULLDOWN_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&config));
}
static bool write_audio(i2s_chan_handle_t tx,const int16_t *data,size_t bytes)
{
    size_t written=0;return i2s_channel_write(tx,data,bytes,&written,2000)==ESP_OK && written==bytes;
}
static void playback_worker(void *arg)
{
    (void)arg;int16_t buffer[320];
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        i2s_chan_handle_t tx=NULL;bool active=false;
        i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
        channel.auto_clear=true;channel.dma_desc_num=8;channel.dma_frame_num=320;
        bool failed=i2s_new_channel(&channel,&tx,NULL)!=ESP_OK;
        i2s_std_config_t config={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
            .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_MONO),
            .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=CONFIG_ASR_SPEAKER_BCLK,.ws=CONFIG_ASR_SPEAKER_WS,
                .dout=CONFIG_ASR_SPEAKER_DOUT,.din=I2S_GPIO_UNUSED}};
        if(!failed)failed=i2s_channel_init_std_mode(tx,&config)!=ESP_OK;
        atomic_store(&play_failed,failed);
        while(!failed && xStreamBufferBytesAvailable(pcm_queue)<10240 && !atomic_load(&synth_done))vTaskDelay(pdMS_TO_TICKS(10));
        for(;;) {
            size_t n=xStreamBufferReceive(pcm_queue,buffer,sizeof(buffer),pdMS_TO_TICKS(20));
            if(n && !failed) {
                for(unsigned i=0;i<n/2;i++)buffer[i]=(int32_t)buffer[i]*CONFIG_ASSISTANT_SPEAKER_VOLUME/100;
                if(!active){failed=i2s_channel_enable(tx)!=ESP_OK;active=!failed;}
                if(!failed)failed=!write_audio(tx,buffer,n);
            }
            if(failed)atomic_store(&play_failed,true);
            if(atomic_load(&synth_done) && !xStreamBufferBytesAvailable(pcm_queue))break;
        }
        if(active) {
            memset(buffer,0,sizeof(buffer));
            for(int i=0;i<8;i++)if(!write_audio(tx,buffer,sizeof(buffer)))atomic_store(&play_failed,true);
            vTaskDelay(pdMS_TO_TICKS(180));i2s_channel_disable(tx);
        }
        if(tx)i2s_del_channel(tx);
        speaker_quiet();
        xSemaphoreGive(play_done);
    }
}
typedef struct { turn_context_t context;unsigned samples;int64_t last_yield; } speech_context_t;
static int on_pcm(const int16_t *input,size_t samples,void *user)
{
    speech_context_t *s=user;
    if(cancelled(&s->context) || atomic_load(&play_failed))return 1;
    s->samples+=samples;const uint8_t *data=(const uint8_t *)input;size_t bytes=samples*2;
    while(bytes) {
        if(cancelled(&s->context) || atomic_load(&play_failed))return 1;
        size_t n=xStreamBufferSend(pcm_queue,data,bytes,pdMS_TO_TICKS(50));data+=n;bytes-=n;
    }
    if(esp_timer_get_time()-s->last_yield>=20000){vTaskDelay(1);s->last_yield=esp_timer_get_time();}
    return 0;
}
static bool speak(const char *text,turn_context_t *context)
{
    if(cancelled(context))return false;
    phase("speaking");display_text(text);atomic_store(&tts_active,true);
    xStreamBufferReset(pcm_queue);atomic_store(&play_failed,false);atomic_store(&synth_done,false);
    speech_context_t speech={.context=*context,.last_yield=esp_timer_get_time()};
    xTaskNotifyGive(play_task);
    int result=lingtts_engine_speak(tts,text,on_pcm,&speech);
    atomic_store(&synth_done,true);xSemaphoreTake(play_done,portMAX_DELAY);
    atomic_store(&tts_active,false);
    ESP_LOGI(TAG,"Playback complete: status=%d samples=%u output_error=%d",result,speech.samples,atomic_load(&play_failed));
    return result==LINGTTS_OK && speech.samples && !atomic_load(&play_failed) && !cancelled(context);
}
static void finish_turn(const char *user,turn_context_t *context)
{
    char reply[ASSISTANT_REPLY]={0},error[256]={0};
    xSemaphoreTake(mutex,portMAX_DELAY);assistant_copy_text(state.user,sizeof(state.user),user);state.reply[0]=0;state.error[0]=0;xSemaphoreGive(mutex);
    phase("thinking");display_text(user[0] ? user : "没有听清，请再说一次");
    bool ok;
    if(!user[0]){assistant_copy_text(reply,sizeof(reply),"没有听清，请再说一次。");ok=true;}
    else ok=assistant_network_call_traced(user,history,reply,sizeof(reply),error,sizeof(error),cancelled,context,&context->timing);
    assistant_timing_t *t=&context->timing;
    if(t->asr_begin_us)ESP_LOGI(TAG,"LATENCY_ASR {\"turn_id\":%u,\"segment_id\":%u,\"samples\":%u,\"window_ms\":%u,\"reason\":%u,\"last_voice_estimate_us\":%lld,\"endpoint_us\":%lld,\"queued_us\":%lld,\"begin_us\":%lld,\"frontend_end_us\":%lld,\"infer_end_us\":%lld,\"end_us\":%lld}",
        t->turn_id,t->segment_id,t->samples,t->window_ms,t->end_reason,(long long)t->last_voice_us,
        (long long)t->endpoint_us,(long long)t->queued_us,(long long)t->asr_begin_us,
        (long long)t->frontend_end_us,(long long)t->infer_end_us,(long long)t->asr_end_us);
    if(ok && !cancelled(context)) {
        xSemaphoreTake(mutex,portMAX_DELAY);assistant_copy_text(state.reply,sizeof(state.reply),reply);xSemaphoreGive(mutex);
        ok=speak(reply,context);
        if(ok && user[0])assistant_history_add(history,user,reply);
        if(!ok && !cancelled(context))assistant_copy_text(error,sizeof(error),"播报失败，请检查扬声器接线后重试");
    }
    if(!cancelled(context)) {
        xSemaphoreTake(mutex,portMAX_DELAY);
        if(error[0]) {
            assistant_copy_text(state.error,sizeof(state.error),error);assistant_copy_text(state.display,sizeof(state.display),error);
        }
        state.resume_after=esp_timer_get_time()+(error[0] ? 2000000 : 300000);
        xSemaphoreGive(mutex);
    }
    if(atomic_load(&epoch)==context->epoch)atomic_store(&turn_closed,false);
}
static void turn_worker(void *arg)
{
    (void)arg;char *combined=psalloc(ASSISTANT_TEXT),*text=psalloc(ASSISTANT_TEXT);
    unsigned last_revision=UINT32_MAX,combined_epoch=0;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        xSemaphoreTake(mutex,portMAX_DELAY);
        assistant_copy_text(text,ASSISTANT_TEXT,state.manual);state.manual[0]=0;
        xSemaphoreGive(mutex);
        if(text[0]) {
            atomic_store(&worker_running,true);turn_context_t c={.epoch=atomic_load(&epoch),.revision=assistant_network_revision()};
            c.timing.turn_id=atomic_fetch_add(&turn_sequence,1)+1;
            if(last_revision!=c.revision){assistant_network_memory_load(history);last_revision=c.revision;}
            finish_turn(text,&c);atomic_store(&worker_running,false);
        }
        for(;;) {
            xSemaphoreTake(mutex,portMAX_DELAY);int slot=lingasr_audio_take(&audio);
            lingasr_audio_job_t job={0};turn_context_t c={0};unsigned reason=0;
            if(slot>=0){job=audio.slots[slot];c.epoch=job_epoch[slot];c.revision=job_revision[slot];reason=job_reason[slot];c.timing=job_timing[slot];}
            xSemaphoreGive(mutex);if(slot<0)break;
            atomic_store(&worker_running,true);
            if(!cancelled(&c)) {
                if(last_revision!=c.revision){assistant_network_memory_load(history);last_revision=c.revision;}
                if(combined_epoch!=c.epoch || !job.continuation){combined[0]=0;combined_epoch=c.epoch;}
                phase("recognizing");int64_t begin=esp_timer_get_time();
                c.timing.asr_begin_us=begin;
                lingasr_request_t request={.pcm=job.pcm,.samples=job.samples,.window_ms=job.window_ms,.cancelled=cancelled,.userdata=&c};
                lingasr_result_t metrics={0};
                int result=lingasr_transcribe(recognizer,&request,text,ASSISTANT_TEXT,&metrics);
                c.timing.frontend_end_us=begin+metrics.frontend_us;
                c.timing.infer_end_us=c.timing.frontend_end_us+metrics.inference_us;
                c.timing.asr_end_us=esp_timer_get_time();
                ESP_LOGI(TAG,"ASR complete: status=%d window=%ums compute=%.1fms",result,job.window_ms,(esp_timer_get_time()-begin)/1000.0);
                if(!result && !cancelled(&c)) {
                    char joined[ASSISTANT_TEXT];
                    if(lingasr_audio_join(joined,sizeof(joined),combined,text)>=0)assistant_copy_text(combined,ASSISTANT_TEXT,joined);
                    else assistant_copy_text(combined,ASSISTANT_TEXT,text);
                    if(reason!=LINGASR_END_PROTECTION)finish_turn(combined,&c);
                    else {display_text(combined);phase("recording");}
                } else if(!cancelled(&c)) {
                    xSemaphoreTake(mutex,portMAX_DELAY);assistant_copy_text(state.error,sizeof(state.error),"语音识别计算失败，请重试");state.resume_after=esp_timer_get_time()+2000000;xSemaphoreGive(mutex);
                    atomic_store(&turn_closed,false);
                }
            }
            xSemaphoreTake(mutex,portMAX_DELAY);lingasr_audio_release(&audio,slot);xSemaphoreGive(mutex);
            atomic_fetch_sub(&pending,1);atomic_store(&worker_running,false);
        }
    }
}
static bool overflow(i2s_chan_handle_t channel,i2s_event_data_t *event,void *context)
{
    (void)channel;(void)event;(void)context;atomic_fetch_add(&overflows,1);return false;
}
static void microphone_worker(void *arg)
{
    (void)arg;int32_t raw[LINGASR_CAPTURE_FRAME];int16_t frame[LINGASR_CAPTURE_FRAME];
    unsigned warm=0,last_epoch=0,last_overflows=0,utterance=0;bool reset=true;
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    for(;;) {
        size_t bytes=0;
        if(i2s_channel_read(rx,raw,sizeof(raw),&bytes,1000)!=ESP_OK || bytes!=sizeof(raw)){set_enabled(false);reset=true;continue;}
        for(unsigned i=0;i<LINGASR_CAPTURE_FRAME;i++)frame[i]=(int16_t)(raw[i]>>16);
        if(warm<400) {
            if(warm>=350)lingasr_capture_observe(capture,frame);
            if(++warm==400){atomic_store(&mic_warm,true);last_overflows=atomic_load(&overflows);ESP_LOGI(TAG,"Microphone warm; assistant ready when Wi-Fi is configured");}
            continue;
        }
        unsigned current=atomic_load(&epoch),lost=atomic_load(&overflows);
        if(current!=last_epoch){lingasr_capture_cancel(capture);atomic_store(&capturing,false);last_epoch=current;reset=true;}
        if(lost!=last_overflows){last_overflows=lost;ESP_LOGE(TAG,"Microphone overflow; monitoring paused");set_enabled(false);reset=true;continue;}
        xSemaphoreTake(mutex,portMAX_DELAY);int64_t after=state.resume_after;xSemaphoreGive(mutex);
        bool armed=atomic_load(&enabled) && !atomic_load(&turn_closed) && !atomic_load(&tts_active) &&
            esp_timer_get_time()>=after && assistant_network_ready();
        if(!armed) {
            if(!reset)lingasr_capture_cancel(capture);
            atomic_store(&capturing,false);
            if(!atomic_load(&tts_active) && !atomic_load(&turn_closed) && esp_timer_get_time()>=after)lingasr_capture_observe(capture,frame);
            reset=true;continue;
        }
        if(reset){lingasr_capture_cancel(capture);reset=false;}
        int event=lingasr_capture_push(capture,frame,pcm);
        if(event<0){set_enabled(false);reset=true;continue;}
        if(event==LINGASR_CAPTURE_STARTED){utterance=atomic_fetch_add(&turn_sequence,1)+1;atomic_store(&capturing,true);phase("recording");}
        if(event==LINGASR_CAPTURE_DISCARDED)atomic_store(&capturing,false);
        if(event!=LINGASR_CAPTURE_READY)continue;
        int64_t endpoint=esp_timer_get_time();
        lingasr_capture_info_t info;ESP_ERROR_CHECK(lingasr_capture_get_info(capture,&info)==LINGASR_OK ? ESP_OK : ESP_FAIL);
        unsigned total_frames=info.buffered_samples/LINGASR_CAPTURE_FRAME,last_voice_frame=info.last_voice_frame;
        unsigned reason=info.reason,rev=assistant_network_revision();
        xSemaphoreTake(mutex,portMAX_DELAY);
        int slot=lingasr_audio_reserve(&audio,info.samples,utterance,info.continuation);
        xSemaphoreGive(mutex);
        if(slot<0){ESP_LOGE(TAG,"Audio queue full; monitoring paused");set_enabled(false);reset=true;continue;}
        lingasr_audio_job_t *j=&audio.slots[slot];
        memcpy(j->pcm,pcm,j->samples*2);memset(j->pcm+j->samples,0,(j->window_ms*16-j->samples)*2);
        xSemaphoreTake(mutex,portMAX_DELAY);
        if(current==atomic_load(&epoch) && atomic_load(&enabled)) {
            job_epoch[slot]=current;job_revision[slot]=rev;job_reason[slot]=reason;
            job_timing[slot]=(assistant_timing_t){.turn_id=utterance,.segment_id=j->sequence,
                .last_voice_us=last_voice_frame ? endpoint-(int64_t)(total_frames-last_voice_frame)*20000 : 0,
                .endpoint_us=endpoint,.queued_us=esp_timer_get_time(),.samples=j->samples,
                .window_ms=j->window_ms,.end_reason=reason};
            if(reason!=LINGASR_END_PROTECTION){atomic_store(&turn_closed,true);atomic_store(&capturing,false);}
            atomic_fetch_add(&pending,1);lingasr_audio_commit(&audio,slot);
        } else j->state=LINGASR_AUDIO_FREE;
        xSemaphoreGive(mutex);lingasr_capture_advance(capture,pcm);xTaskNotifyGive(turn_task);
    }
}
static void supervisor(void *arg)
{
    (void)arg;lingasr_button_t button={0};unsigned down_ms=0;bool held=false;
    for(;;) {
        bool down=gpio_get_level(CONFIG_ASR_BUTTON_GPIO)==0;
        if(lingasr_button_poll(&button,down))set_enabled(!atomic_load(&enabled));
        if(!down){down_ms=0;held=false;}
        else if(!held && (down_ms+=20)>=2000) {
            held=true;set_enabled(false);assistant_network_enter_config_mode();
        }
        if(!atomic_load(&worker_running) && !atomic_load(&pending) && !atomic_load(&capturing) && !atomic_load(&turn_closed)) {
            xSemaphoreTake(mutex,portMAX_DELAY);bool elapsed=esp_timer_get_time()>=state.resume_after;xSemaphoreGive(mutex);
            if(elapsed) {
                if(assistant_network_config_mode()) {
                    phase("configuring");display_text("配置热点模式\nLingASR-Assistant\n192.168.4.1");
                }
                else if(!atomic_load(&enabled)){phase("paused");display_text("已暂停，按BOOT继续");}
                else if(!atomic_load(&mic_warm)){phase("warming");display_text("麦克风预热中");}
                else if(assistant_network_ready()){phase("ready");display_text(READY);}
                else if(assistant_network_configured()) {
                    phase("connecting");display_text("正在连接Wi-Fi\n请在配置页检查网络");
                } else {
                    phase("configuring");display_text("请连接配置热点\n" CONFIG_ASSISTANT_AP_SSID "\n192.168.4.1");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void app_main(void)
{
    speaker_quiet();mutex=xSemaphoreCreateMutex();play_done=xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(mutex && play_done ? ESP_OK : ESP_ERR_NO_MEM);
    lingasr_config_t asr_config;lingasr_default_config(&asr_config);
    ESP_ERROR_CHECK(lingasr_create(&asr_config,&recognizer)==LINGASR_OK ? ESP_OK : ESP_FAIL);
    pcm=psalloc(LINGASR_CAPTURE_SAMPLES*2);history=psalloc(sizeof(*history));
    ESP_ERROR_CHECK(lingasr_capture_create(&capture)==LINGASR_OK ? ESP_OK : ESP_FAIL);
    for(unsigned i=0;i<LINGASR_AUDIO_SLOTS;i++)audio.slots[i].pcm=psalloc(LINGASR_CAPTURE_SAMPLES*2);
    lingasr_memory_t asr_memory;lingasr_get_memory(recognizer,&asr_memory);
    lingtts_engine_cfg_t cfg;lingtts_engine_default_cfg(&cfg);cfg.flags=LINGTTS_ENGINE_NO_COPY;
    ESP_ERROR_CHECK(lingtts_engine_create(&tts,&cfg)==LINGTTS_OK ? ESP_OK : ESP_FAIL);
    lingtts_engine_mem_t memory;lingtts_engine_get_mem(tts,&memory);
    ESP_LOGI(TAG,"ASR arena=%u workspace=%u; TTS arenas=%u copied=%u; free internal=%u PSRAM=%u",
             (unsigned)asr_memory.activation_bytes,(unsigned)asr_memory.workspace_bytes,(unsigned)(memory.tts_arena+memory.fe_arena),
             (unsigned)memory.copied,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    pcm_queue=xStreamBufferCreateStatic(65537,1,psalloc(65537),&pcm_control);ESP_ERROR_CHECK(pcm_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(asr_display_init(screen_snapshot));
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(playback_worker,"assistant_audio",4096,NULL,6,&play_task,0)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(assistant_network_start(status_snapshot,submit_text,set_enabled));
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(turn_worker,"assistant_turn",20480,NULL,3,&turn_task,1)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);channel.dma_desc_num=8;channel.dma_frame_num=256;
    ESP_ERROR_CHECK(i2s_new_channel(&channel,NULL,&rx));
    i2s_std_config_t config={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_MONO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=CONFIG_ASR_MIC_BCLK,.ws=CONFIG_ASR_MIC_WS,.din=CONFIG_ASR_MIC_DIN,.dout=I2S_GPIO_UNUSED}};
    config.slot_cfg.slot_mask=I2S_STD_SLOT_LEFT;ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx,&config));
    i2s_event_callbacks_t callbacks={.on_recv_q_ovf=overflow};ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx,&callbacks,NULL));
    gpio_config_t button={.pin_bit_mask=1ULL<<CONFIG_ASR_BUTTON_GPIO,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};ESP_ERROR_CHECK(gpio_config(&button));
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(microphone_worker,"assistant_mic",8192,NULL,4,NULL,0)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(supervisor,"assistant_ui",3072,NULL,3,NULL,0)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG,"Assistant boot complete; configure http://192.168.4.1; BOOT pauses/resumes");
}
