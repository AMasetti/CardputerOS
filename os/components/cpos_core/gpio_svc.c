/* GPIO and PWM exposed to apps. IR pin needs the IR permission, the Grove/EXT pins the GPIO one. */
#include "app_ctx.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "hal.h"

static int pin_ok(int pin) {
    if (pin == PIN_IR_TX) return cpos_has_perm(CPOS_PERM_IR);
    /* Grove (1,2) and EXT header pins */
    static const int ext[] = {1, 2, 3, 4, 5, 6, 7, 13, 15, 40, 39, 14, 12, 44};
    for (unsigned i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (ext[i] == pin) return cpos_has_perm(CPOS_PERM_GPIO);
    return 0;
}

static void g_pin_mode(int pin, int mode) {
    if (!pin_ok(pin)) return;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, mode == 1 ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    if (mode == 2) gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
}
static void g_write(int pin, int level) {
    if (pin_ok(pin)) gpio_set_level(pin, level);
}
static int g_read(int pin) { return pin_ok(pin) ? gpio_get_level(pin) : 0; }

static int s_bits = 8;
static int g_pwm_setup(int ch, uint32_t freq, int bits) {
    if (ch < 0 || ch > 5) return 0;
    s_bits = bits > 14 ? 14 : (bits < 1 ? 1 : bits);
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = s_bits, .timer_num = LEDC_TIMER_0,
                             .freq_hz = freq ? freq : 1000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&t);
    return (int)freq;
}
static void g_pwm_attach(int pin, int ch) {
    if (!pin_ok(pin) || ch < 0 || ch > 5) return;
    ledc_channel_config_t c = {.gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = ch, .timer_sel = LEDC_TIMER_0, .duty = 0};
    ledc_channel_config(&c);
    if (g_app) g_app->pwm_pin[ch] = pin;
}
static void g_pwm_write(int ch, uint32_t duty) {
    if (ch < 0 || ch > 5 || !g_app || g_app->pwm_pin[ch] < 0) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

void cpos_gpio_fill(cpos_gpio_api_t *g) {
    g->pin_mode = g_pin_mode;
    g->write = g_write;
    g->read = g_read;
    g->pwm_setup = g_pwm_setup;
    g->pwm_attach = g_pwm_attach;
    g->pwm_write = g_pwm_write;
}

void cpos_app_cleanup_gpio(app_ctx_t *a) {
    for (int ch = 0; ch < 6; ch++) {
        if (a->pwm_pin[ch] < 0) continue;
        ledc_stop(LEDC_LOW_SPEED_MODE, ch, 0);
        gpio_reset_pin(a->pwm_pin[ch]);
        a->pwm_pin[ch] = -1;
    }
}
