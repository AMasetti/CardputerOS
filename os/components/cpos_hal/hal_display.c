/* ST7789V2 135x240 on SPI2, driven through esp_lcd, landscape 240x135. */
#include "hal.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define BAND_ROWS 16
#define LCD_W 240
#define LCD_H 135

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_slots; /* counts free DMA buffers */
static SemaphoreHandle_t s_lock;
static uint16_t *s_dma[2];

static bool on_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_slots, &woken);
    return woken == pdTRUE;
}

void hal_display_brightness(uint8_t v) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, v);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
}

void hal_display_init(void) {
    s_slots = xSemaphoreCreateCounting(2, 2);
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < 2; i++) s_dma[i] = heap_caps_malloc(LCD_W * BAND_ROWS * 2, MALLOC_CAP_DMA);

    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                             .timer_num = LEDC_TIMER_3, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {.gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_7,
                                .timer_sel = LEDC_TIMER_3, .duty = 0, .hpoint = 0};
    ledc_channel_config(&ch);

    spi_bus_config_t bus = {.mosi_io_num = PIN_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_LCD_SCLK,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_W * BAND_ROWS * 2 + 16};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {.dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS, .pclk_hz = 40 * 1000 * 1000,
                                            .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 2,
                                            .on_color_trans_done = on_done};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);
    esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y);

    /* start black, then light the backlight */
    static uint16_t black[LCD_W * BAND_ROWS];
    for (int y = 0; y < LCD_H; y += BAND_ROWS)
        hal_display_blit(0, y, LCD_W, (y + BAND_ROWS > LCD_H) ? LCD_H - y : BAND_ROWS, black, LCD_W);
    esp_lcd_panel_disp_on_off(s_panel, true);
    hal_display_brightness(160);
    ESP_LOGI(TAG, "ST7789 ready %dx%d", LCD_W, LCD_H);
}

void hal_display_blit(int x, int y, int w, int h, const uint16_t *pix, int stride) {
    if (!s_panel || w <= 0 || h <= 0) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int k = 0;
    for (int r = 0; r < h; r += BAND_ROWS) {
        int rows = (r + BAND_ROWS > h) ? h - r : BAND_ROWS;
        xSemaphoreTake(s_slots, portMAX_DELAY); /* a DMA buffer is free again */
        uint16_t *dst = s_dma[k];
        for (int i = 0; i < rows; i++) {
            const uint16_t *src = pix + (r + i) * stride;
            uint16_t *d = dst + i * w;
            for (int q = 0; q < w; q++) d[q] = __builtin_bswap16(src[q]);
        }
        esp_lcd_panel_draw_bitmap(s_panel, x, y + r, x + w, y + r + rows, dst);
        k ^= 1;
    }
    /* wait for the last two transfers so the caller may reuse its buffer */
    xSemaphoreTake(s_slots, portMAX_DELAY);
    xSemaphoreTake(s_slots, portMAX_DELAY);
    xSemaphoreGive(s_slots);
    xSemaphoreGive(s_slots);
    xSemaphoreGive(s_lock);
}
