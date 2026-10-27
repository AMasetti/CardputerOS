/* Keyboard: TCA8418 matrix controller on the internal I2C bus, polled. */
#include "hal.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TCA_ADDR 0x34
#define TCA_CFG 0x01
#define TCA_INT_STAT 0x02
#define TCA_KEY_LCK_EC 0x03
#define TCA_KEY_EVENT_A 0x04
#define TCA_KP_GPIO1 0x1D
#define TCA_KP_GPIO2 0x1E
#define TCA_KP_GPIO3 0x1F

static const char *TAG = "input";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_tca;
static bool s_ok;

/* The physical keyboard is 4 rows x 14 columns. {value, value with shift}. */
#define K_TAB 0x09
#define K_ENTER 0x0A
#define K_BS 0x08
typedef struct { char a, b; } kv_t;
static const kv_t KEYMAP[4][14] = {
    {{'`','~'},{'1','!'},{'2','@'},{'3','#'},{'4','$'},{'5','%'},{'6','^'},{'7','&'},{'8','*'},{'9','('},{'0',')'},{'-','_'},{'=','+'},{K_BS,K_BS}},
    {{K_TAB,K_TAB},{'q','Q'},{'w','W'},{'e','E'},{'r','R'},{'t','T'},{'y','Y'},{'u','U'},{'i','I'},{'o','O'},{'p','P'},{'[','{'},{']','}'},{'\\','|'}},
    {{0,0},{0,0},{'a','A'},{'s','S'},{'d','D'},{'f','F'},{'g','G'},{'h','H'},{'j','J'},{'k','K'},{'l','L'},{';',':'},{'\'','"'},{K_ENTER,K_ENTER}},
    {{0,0},{0,0},{0,0},{'z','Z'},{'x','X'},{'c','C'},{'v','V'},{'b','B'},{'n','N'},{'m','M'},{',','<'},{'.','>'},{'/','?'},{' ',' '}},
};
/* modifier positions: fn (2,0) shift (2,1) ctrl (3,0) opt (3,1) alt (3,2) */

static bool s_down[4][14];
static uint32_t s_seq;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

i2c_master_bus_handle_t hal_i2c(void) { return s_bus; }

static esp_err_t rd(uint8_t reg, uint8_t *v) { return i2c_master_transmit_receive(s_tca, &reg, 1, v, 1, 50); }
static esp_err_t wr(uint8_t reg, uint8_t v) { uint8_t b[2] = {reg, v}; return i2c_master_transmit(s_tca, b, 2, 50); }

void cpos_hal_init(void) {
    /* The ADV leaves CS lines of the cap connectors floating; hold them high so the SD bus is clean. */
    static const int highs[] = {3, 4, 5, 6, 13, 15};
    for (unsigned i = 0; i < sizeof(highs) / sizeof(highs[0]); i++) {
        gpio_reset_pin(highs[i]);
        gpio_set_direction(highs[i], GPIO_MODE_OUTPUT);
        gpio_set_level(highs[i], 1);
    }
    gpio_reset_pin(PIN_BTN_A);
    gpio_set_direction(PIN_BTN_A, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BTN_A, GPIO_PULLUP_ONLY);

    i2c_master_bus_config_t bc = {.i2c_port = I2C_NUM_0, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
                                  .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &s_bus));
}

static void apply_event(int v, bool pressed) {
    int u = v % 10, t = v / 10;
    if (u < 1 || u > 8 || t > 6) return;
    int row = (u - 1) & 3, col = (t << 1) | ((u - 1) >> 2);
    portENTER_CRITICAL(&s_mux);
    s_down[row][col] = pressed;
    s_seq++;
    portEXIT_CRITICAL(&s_mux);
}

static void kbd_task(void *arg) {
    for (;;) {
        uint8_t n = 0;
        if (rd(TCA_KEY_LCK_EC, &n) == ESP_OK) {
            n &= 0x0F;
            while (n--) {
                uint8_t ev = 0;
                if (rd(TCA_KEY_EVENT_A, &ev) != ESP_OK || ev == 0) break;
                apply_event(ev & 0x7F, ev & 0x80);
            }
            wr(TCA_INT_STAT, 0x1F);
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void hal_kbd_init(void) {
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TCA_ADDR, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(s_bus, &dc, &s_tca) != ESP_OK) return;
    uint8_t dummy;
    if (rd(TCA_CFG, &dummy) != ESP_OK) {
        ESP_LOGE(TAG, "TCA8418 not found at 0x%02X", TCA_ADDR);
        return;
    }
    wr(TCA_KP_GPIO1, 0x7F); /* rows 0..6 */
    wr(TCA_KP_GPIO2, 0xFF); /* columns 0..7 */
    wr(TCA_KP_GPIO3, 0x00);
    uint8_t n;
    while (rd(TCA_KEY_LCK_EC, &n) == ESP_OK && (n & 0x0F)) rd(TCA_KEY_EVENT_A, &dummy);
    wr(TCA_INT_STAT, 0x1F);
    wr(TCA_CFG, 0x01);
    s_ok = true;
    xTaskCreate(kbd_task, "kbd", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "TCA8418 keyboard ready");
}

void hal_kbd_get(cpos_kbd_t *o) {
    bool d[4][14];
    portENTER_CRITICAL(&s_mux);
    memcpy(d, s_down, sizeof(d));
    o->seq = s_seq;
    portEXIT_CRITICAL(&s_mux);
    memset(o->keys, 0, sizeof(o->keys));
    o->count = 0;
    o->fn = d[2][0];
    o->shift = d[2][1];
    o->ctrl = d[3][0];
    o->opt = d[3][1];
    o->alt = d[3][2];
    o->tab = d[1][0];
    o->del = d[0][13];
    o->enter = d[2][13];
    o->space = d[3][13];
    o->modifiers = (o->ctrl ? 1 : 0) | (o->shift ? 2 : 0) | (o->alt ? 4 : 0) | (o->opt ? 8 : 0) | (o->fn ? 16 : 0);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 14; c++) {
            if (!d[r][c]) continue;
            char ch = o->shift ? KEYMAP[r][c].b : KEYMAP[r][c].a;
            if (!ch || ch == K_TAB || ch == K_ENTER || ch == K_BS) continue;
            if (o->count < CPOS_KBD_MAX_KEYS) o->keys[o->count++] = ch;
        }
    }
}

bool hal_kbd_esc(void) {
    bool fn, bt;
    portENTER_CRITICAL(&s_mux);
    fn = s_down[2][0];
    bt = s_down[0][0];
    portEXIT_CRITICAL(&s_mux);
    return fn && bt;
}
