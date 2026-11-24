/* Copyright (c) 2026 LingCoreX. MIT license. */
#include <stdio.h>
#include "lingasr.h"
#include "lingasr_capture.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void recognize(void *unused)
{
    (void)unused;
    lingasr_engine_t *engine=NULL;
    lingasr_status_t status=lingasr_create(NULL,&engine);
    if(status!=LINGASR_OK){ESP_LOGE("microphone","Engine init failed: %d",status);vTaskDelete(NULL);return;}
    int16_t *pcm=heap_caps_malloc(64000*sizeof(*pcm),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(pcm ? ESP_OK : ESP_ERR_NO_MEM);
    i2s_chan_handle_t rx;
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&channel,NULL,&rx));
    i2s_std_config_t config={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_MONO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=5,.ws=4,.din=6,.dout=I2S_GPIO_UNUSED}};
    config.slot_cfg.slot_mask=I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx,&config));
    gpio_config_t button_config={.pin_bit_mask=1ULL,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&button_config));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    lingasr_button_t button={0};int32_t frame[320];unsigned warm=0,recorded=0;bool recording=false;
    printf("LingASR Preview %s: microphone warming for eight seconds\n",lingasr_version());
    for(;;){
        size_t bytes=0;
        esp_err_t err=i2s_channel_read(rx,frame,sizeof(frame),&bytes,1000);
        if(err!=ESP_OK || bytes!=sizeof(frame)){
            ESP_LOGE("microphone","Incomplete audio frame; discarding capture");recording=false;recorded=0;continue;
        }
        if(warm<400){if(++warm==400)printf("Ready: release BOOT, then press to record four seconds\n");continue;}
        if(lingasr_button_poll(&button,gpio_get_level(0)==0) && !recording){recorded=0;recording=true;printf("Speak now (four seconds)\n");}
        if(!recording)continue;
        for(unsigned i=0;i<320;i++)pcm[recorded+i]=(int16_t)(frame[i]>>16);
        recorded+=320;if(recorded<64000)continue;
        ESP_ERROR_CHECK(i2s_channel_disable(rx));
        char text[1024];lingasr_result_t metrics;
        lingasr_request_t request={.pcm=pcm,.samples=64000};
        status=lingasr_transcribe(engine,&request,text,sizeof(text),&metrics);
        if(status==LINGASR_OK)printf("Text: %s\nCompute: %.1f ms, RTF: %.3f\n",text,metrics.total_us/1000.0,metrics.total_us/4000000.0);
        else printf("Recognition failed: %d\n",status);
        recording=false;recorded=0;ESP_ERROR_CHECK(i2s_channel_enable(rx));
        printf("Ready: press BOOT to record again\n");
    }
}
void app_main(void)
{
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(recognize,"recognize",20480,NULL,3,NULL,1)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
