#include "sdkconfig.h"
#include "asr_display.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static atomic_bool ready;
bool asr_display_ready(void) { return atomic_load(&ready); }

#ifdef CONFIG_ASR_LCD_ENABLE
enum { STRIP_ROWS=16, PAGE_US=4000000, DRAFT_CHAR_US=150000 };
static const char *TAG="asr_lcd";
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t transfer_done;
static SemaphoreHandle_t result_ready;
static uint16_t *frame, *strip;
static asr_display_snapshot_fn snapshot_fn;

void asr_display_notify(void)
{
    if(result_ready && asr_display_ready())xSemaphoreGive(result_ready);
}

static bool transferred(esp_lcd_panel_io_handle_t handle, esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)handle; (void)event; (void)ctx;
    BaseType_t wake=pdFALSE;
    xSemaphoreGiveFromISR(transfer_done,&wake);
    return wake==pdTRUE;
}

static esp_err_t flush(unsigned first,unsigned stop)
{
    for (unsigned y=first; y<stop; y+=STRIP_ROWS) {
        unsigned rows=stop-y;
        if (rows>STRIP_ROWS) rows=STRIP_ROWS;
        for (unsigned i=0; i<rows*240; i++) strip[i]=__builtin_bswap16(frame[y*240+i]);
        esp_err_t err=esp_lcd_panel_draw_bitmap(panel,0,y,240,y+rows,strip);
        if (err!=ESP_OK) return err;
        /* Never overwrite a DMA buffer until its completion callback fires. */
        if (xSemaphoreTake(transfer_done,pdMS_TO_TICKS(1000))!=pdTRUE) return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void display_worker(void *arg)
{
    (void)arg;
    asr_screen_state_t previous={0}, current;
    bool first=true;
    unsigned pages=1, last_page=0;
    int64_t page_started=esp_timer_get_time();
    int64_t reveal_at=0;
    for (;;) {
        memset(&current,0,sizeof(current));
        snapshot_fn(&current);
        int64_t now=esp_timer_get_time();
        bool new_draft=current.draft && (!previous.draft || current.id!=previous.id);
        bool advance=current.draft && (new_draft || now>=reveal_at);
        bool draft_pending=asr_screen_draft_step(&current,&previous,advance);
        if(advance)reveal_at=now+DRAFT_CHAR_US;
        if(!current.draft)reveal_at=0;
        bool content_changed=first || new_draft || strcmp(current.text,previous.text);
        if(!result_ready)
            content_changed=content_changed || current.id!=previous.id ||
                            strcmp(current.state,previous.state) || strcmp(current.error,previous.error);
        if (content_changed) page_started=esp_timer_get_time();
        unsigned page=current.draft ? asr_screen_page_count(&current)-1
                                    : (unsigned)((esp_timer_get_time()-page_started)/PAGE_US)%pages;
        if (content_changed || (!result_ready && memcmp(&current,&previous,sizeof(current))) || page!=last_page) {
#ifdef CONFIG_ASR_SEGMENT_SELFTEST
            int64_t draw_started=esp_timer_get_time();
#endif
            unsigned top=0,stop=ASR_SCREEN_HEIGHT;
            if(!first && page==last_page)asr_screen_append_rows(&current,&previous,page,&top,&stop);
            pages=asr_screen_render(frame,&current,page);
            esp_err_t err=flush(top,stop);
            if (err!=ESP_OK) {
                atomic_store(&ready,false);
                gpio_set_level(CONFIG_ASR_LCD_BL,!CONFIG_ASR_LCD_BL_LEVEL);
                ESP_LOGE(TAG,"LCD transfer failed: %s; ASR remains available",esp_err_to_name(err));
                break;
            }
            if (first) gpio_set_level(CONFIG_ASR_LCD_BL,CONFIG_ASR_LCD_BL_LEVEL);
            if(advance && top==0 && stop==ASR_SCREEN_HEIGHT)
                reveal_at=esp_timer_get_time()+DRAFT_CHAR_US;
            previous=current;
            last_page=page;
            first=false;
            if(result_ready && new_draft)
                ESP_LOGI(TAG,"ASR_LCD_PREVIEW segment=%u t=%lld",current.id,esp_timer_get_time()/1000);
#ifdef CONFIG_ASR_SEGMENT_SELFTEST
            if(result_ready && current.id && content_changed){
                const char *body=current.draft ? strchr(current.text,'\n') : NULL;
                ESP_LOGI(TAG,"ASR_LCD_TEXT %s %u %s",current.draft ? "draft" : "final",
                         current.id,body ? body+1 : current.text);
                if(current.draft)ESP_LOGI(TAG,"ASR_LCD_DRAFT_STEP segment=%u t=%lld",
                                         current.id,esp_timer_get_time()/1000);
            }
            if(result_ready && current.id && !current.draft && content_changed)
                ESP_LOGI(TAG,"ASR_LCD_RESULT %u %lld %lld",current.id,draw_started,esp_timer_get_time());
#endif
        }
        TickType_t timeout=result_ready ? portMAX_DELAY : pdMS_TO_TICKS(200);
        if(draft_pending){
            int64_t remaining=reveal_at-esp_timer_get_time();
            if(remaining<=0)continue;
            unsigned remaining_ms=(remaining+999)/1000;
            timeout=pdMS_TO_TICKS(remaining_ms+portTICK_PERIOD_MS-1);
        }else if(result_ready && !current.draft && pages>1){
            int64_t elapsed=esp_timer_get_time()-page_started;
            if((unsigned)(elapsed/PAGE_US)%pages!=last_page)continue;
            unsigned remaining_ms=(PAGE_US-elapsed%PAGE_US+999)/1000;
            timeout=pdMS_TO_TICKS(remaining_ms+portTICK_PERIOD_MS-1);
        }
        if(result_ready)xSemaphoreTake(result_ready,timeout);
        else vTaskDelay(timeout);
    }
    /* Keep buffers alive if a timed-out transaction is still in flight. */
    vTaskDelete(NULL);
}

static esp_err_t display_init(asr_display_snapshot_fn snapshot,bool event_driven)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    gpio_config_t backlight={.pin_bit_mask=1ULL<<CONFIG_ASR_LCD_BL,.mode=GPIO_MODE_OUTPUT};
    esp_err_t err=gpio_config(&backlight);
    if (err!=ESP_OK) return err;
    gpio_set_level(CONFIG_ASR_LCD_BL,!CONFIG_ASR_LCD_BL_LEVEL);
    frame=heap_caps_malloc(240*240*sizeof(*frame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    strip=heap_caps_malloc(240*STRIP_ROWS*sizeof(*strip),MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA);
    transfer_done=xSemaphoreCreateBinary();
    if(event_driven)result_ready=xSemaphoreCreateBinary();
    bool bus_initialized=false;
    if (!frame || !strip || !transfer_done || (event_driven && !result_ready)) { err=ESP_ERR_NO_MEM; goto failed; }
    spi_bus_config_t bus={
        .sclk_io_num=CONFIG_ASR_LCD_SCLK,.mosi_io_num=CONFIG_ASR_LCD_MOSI,.miso_io_num=-1,
        .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=240*STRIP_ROWS*2,
    };
    err=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO);
    if (err!=ESP_OK) goto failed;
    bus_initialized=true;
    esp_lcd_panel_io_spi_config_t io_config={
        .cs_gpio_num=CONFIG_ASR_LCD_CS,.dc_gpio_num=CONFIG_ASR_LCD_DC,.spi_mode=0,
        .pclk_hz=20000000,.trans_queue_depth=1,.lcd_cmd_bits=8,.lcd_param_bits=8,
        .on_color_trans_done=transferred,
    };
    err=esp_lcd_new_panel_io_spi(SPI2_HOST,&io_config,&io);
    if (err!=ESP_OK) goto failed;
    esp_lcd_panel_dev_config_t panel_config={
        .reset_gpio_num=CONFIG_ASR_LCD_RST,.bits_per_pixel=16,
#ifdef CONFIG_ASR_LCD_BGR
        .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,
#endif
    };
    err=esp_lcd_new_panel_st7789(io,&panel_config,&panel);
    if (err!=ESP_OK) goto failed;
    if ((err=esp_lcd_panel_reset(panel))!=ESP_OK || (err=esp_lcd_panel_init(panel))!=ESP_OK) goto failed;
    err=esp_lcd_panel_invert_color(panel,CONFIG_ASR_LCD_INVERT_LEVEL);
    if (err!=ESP_OK) goto failed;
    err=esp_lcd_panel_swap_xy(panel,CONFIG_ASR_LCD_SWAP_LEVEL);
    if (err!=ESP_OK) goto failed;
    err=esp_lcd_panel_mirror(panel,CONFIG_ASR_LCD_MIRROR_X_LEVEL,CONFIG_ASR_LCD_MIRROR_Y_LEVEL);
    if (err!=ESP_OK) goto failed;
    err=esp_lcd_panel_set_gap(panel,CONFIG_ASR_LCD_X_GAP,CONFIG_ASR_LCD_Y_GAP);
    if (err!=ESP_OK) goto failed;
    err=esp_lcd_panel_disp_on_off(panel,true);
    if (err!=ESP_OK) goto failed;
    snapshot_fn=snapshot;
    atomic_store(&ready,true);
    if (xTaskCreatePinnedToCore(display_worker,"lcd",6144,NULL,1,NULL,0)!=pdPASS) {
        atomic_store(&ready,false);
        err=ESP_ERR_NO_MEM;
        goto failed;
    }
    ESP_LOGI(TAG,"ST7789 240x240, SPI2 20MHz, SCLK=%d MOSI=%d RST=%d DC=%d CS=%d BL=%d",
             CONFIG_ASR_LCD_SCLK,CONFIG_ASR_LCD_MOSI,CONFIG_ASR_LCD_RST,
             CONFIG_ASR_LCD_DC,CONFIG_ASR_LCD_CS,CONFIG_ASR_LCD_BL);
    return ESP_OK;
failed:
    if (panel) { esp_lcd_panel_del(panel); panel=NULL; }
    if (io) { esp_lcd_panel_io_del(io); io=NULL; }
    if (bus_initialized) spi_bus_free(SPI2_HOST);
    if (transfer_done) { vSemaphoreDelete(transfer_done); transfer_done=NULL; }
    if (result_ready) { vSemaphoreDelete(result_ready); result_ready=NULL; }
    free(frame); frame=NULL;
    free(strip); strip=NULL;
    return err;
}

esp_err_t asr_display_init(asr_display_snapshot_fn snapshot) { return display_init(snapshot,false); }
esp_err_t asr_display_init_event_driven(asr_display_snapshot_fn snapshot) { return display_init(snapshot,true); }
#else
esp_err_t asr_display_init(asr_display_snapshot_fn snapshot) { (void)snapshot; return ESP_OK; }
esp_err_t asr_display_init_event_driven(asr_display_snapshot_fn snapshot) { return asr_display_init(snapshot); }
void asr_display_notify(void) {}
#endif
