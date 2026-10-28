#include "hal.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#ifdef CONFIG_CPOS_EMULATOR
/* the emulator has no ADC calibration hardware: report a fixed battery and keep esp_adc out of the image */
int hal_batt_mv(void) { return 3900; }
#else
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

static void adc_setup(void) {
    if (s_adc) return;
    adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&u, &s_adc) != ESP_OK) return;
    adc_oneshot_chan_cfg_t c = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_9, &c); /* GPIO10 */
    adc_cali_curve_fitting_config_t cc = {.unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_9, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_cali_create_scheme_curve_fitting(&cc, &s_cali);
}

int hal_batt_mv(void) {
    adc_setup();
    if (!s_adc) return 0;
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv = 0;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_9, &raw) != ESP_OK) continue;
        if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);
        else mv = raw * 3100 / 4095;
        sum += mv;
        n++;
    }
    return n ? (sum / n) * 2 : 0; /* resistor divider 1:2 */
}

#endif

int hal_batt_pct(void) {
    float v = (float)hal_batt_mv();
    float p = (v - 3300.0f) / (4150.0f - 3350.0f) * 100.0f;
    if (p < 1) p = 1;
    if (p > 100) p = 100;
    return (int)p;
}

int hal_btn_a(void) { return gpio_get_level(PIN_BTN_A) == 0; }
