/* ST7789V2 135x240 on SPI2, driven through esp_lcd, landscape 240x135. */
#include "hal.h"
#include "driver/gpio.h"
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
#include "nvs.h"
/* panel orientation: bit0 mirror x, bit1 mirror y; gap = window offset inside the controller RAM. Saved in NVS
 * so a different panel batch can be corrected on the device itself (System > Display setup). */
static int s_flip = 1, s_gap_x = LCD_GAP_X, s_gap_y = LCD_GAP_Y;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_slots; /* counts free DMA buffers */
static SemaphoreHandle_t s_lock;
static uint16_t *s_dma[2];

static bool on_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_slots, &woken);
    return woken == pdTRUE;
}

#ifdef CONFIG_CPOS_EMULATOR
#include "mbedtls/base64.h"
#include <stdio.h>
static uint16_t s_shadow[LCD_W * LCD_H];

void hal_display_init(void) {}
void hal_display_brightness(uint8_t v) {}
void hal_display_get_orient(int *flip, int *gx, int *gy) { *flip = 1; *gx = 40; *gy = 53; }
void hal_display_set_orient(int flip, int gx, int gy, int save) {}
void hal_display_blit(int x, int y, int w, int h, const uint16_t *pix, int stride) {
    for (int r = 0; r < h && y + r < LCD_H; r++)
        for (int q = 0; q < w && x + q < LCD_W; q++) s_shadow[(y + r) * LCD_W + x + q] = pix[r * stride + q];
}
/* print the last frame run-length encoded (count-1, lo, hi) and base64'd so a script can rebuild it */
static unsigned char s_acc[90];
static size_t s_acc_n;
static void emit(unsigned char byte, int flush) {
    if (!flush) s_acc[s_acc_n++] = byte;
    if (s_acc_n == sizeof(s_acc) || (flush && s_acc_n)) {
        unsigned char out[128];
        size_t n = 0;
        mbedtls_base64_encode(out, sizeof(out), &n, s_acc, s_acc_n);
        printf("F:%.*s\n", (int)n, out);
        s_acc_n = 0;
    }
}
void hal_display_dump(const char *name) {
    printf("FRAME %s\n", name);
    s_acc_n = 0;
    size_t total = LCD_W * LCD_H;
    for (size_t i = 0; i < total;) {
        size_t run = 1;
        while (i + run < total && run < 256 && s_shadow[i + run] == s_shadow[i]) run++;
        emit((unsigned char)(run - 1), 0);
        emit(s_shadow[i] & 0xFF, 0);
        emit(s_shadow[i] >> 8, 0);
        i += run;
    }
    emit(0, 1);
    printf("ENDFRAME\n");
}
#else
/* The ADV backlight stays dark when PWM-driven from LEDC (verified on hardware), so it is plain on/off. */
void hal_display_brightness(uint8_t v) {
    if (!s_panel) return;
    gpio_set_level(PIN_LCD_BL, v ? 1 : 0);
}

void hal_display_init(void) {
    s_slots = xSemaphoreCreateCounting(2, 2);
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < 2; i++) s_dma[i] = heap_caps_malloc(LCD_W * BAND_ROWS * 2, MALLOC_CAP_DMA);

    gpio_reset_pin(PIN_LCD_BL);
    gpio_set_direction(PIN_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_BL, 0);

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
    nvs_handle_t nh;
    if (nvs_open("display", NVS_READONLY, &nh) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(nh, "flip", &v) == ESP_OK) s_flip = v & 3;
        if (nvs_get_i32(nh, "gx", &v) == ESP_OK) s_gap_x = v;
        if (nvs_get_i32(nh, "gy", &v) == ESP_OK) s_gap_y = v;
        nvs_close(nh);
    }
    esp_lcd_panel_mirror(s_panel, s_flip & 1, s_flip & 2);
    esp_lcd_panel_set_gap(s_panel, s_gap_x, s_gap_y);

    /* start black, then light the backlight */
    static uint16_t black[LCD_W * BAND_ROWS];
    for (int y = 0; y < LCD_H; y += BAND_ROWS)
        hal_display_blit(0, y, LCD_W, (y + BAND_ROWS > LCD_H) ? LCD_H - y : BAND_ROWS, black, LCD_W);
    esp_lcd_panel_disp_on_off(s_panel, true);
    hal_display_brightness(160);
    ESP_LOGI(TAG, "ST7789 ready %dx%d", LCD_W, LCD_H);
}

void hal_display_get_orient(int *flip, int *gx, int *gy) { *flip = s_flip; *gx = s_gap_x; *gy = s_gap_y; }

void hal_display_set_orient(int flip, int gx, int gy, int save) {
    s_flip = flip & 3; s_gap_x = gx; s_gap_y = gy;
    if (s_panel) {
        esp_lcd_panel_mirror(s_panel, s_flip & 1, s_flip & 2);
        esp_lcd_panel_set_gap(s_panel, s_gap_x, s_gap_y);
    }
    nvs_handle_t nh;
    if (save && nvs_open("display", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_i32(nh, "flip", s_flip); nvs_set_i32(nh, "gx", s_gap_x); nvs_set_i32(nh, "gy", s_gap_y);
        nvs_commit(nh); nvs_close(nh);
    }
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
#endif
