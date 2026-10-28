/* BMI270 accelerometer + gyroscope on the internal I2C bus (address 0x69, 0x68 as a fallback). */
#include "app_ctx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal.h"
#include <string.h>

extern const uint8_t bmi270_config_blob[8192];

#define R_CHIP_ID 0x00
#define R_INTERNAL_STATUS 0x21
#define R_ACC_X 0x0C
#define R_ACC_CONF 0x40
#define R_GYR_CONF 0x42
#define R_INIT_CTRL 0x59
#define R_INIT_ADDR_0 0x5B
#define R_INIT_DATA 0x5E
#define R_PWR_CONF 0x7C
#define R_PWR_CTRL 0x7D
#define R_CMD 0x7E

static const char *TAG = "imu";
static i2c_master_dev_handle_t s_dev;
static int s_state; /* 0 untried, 1 ready, -1 absent */
static float s_acc[3], s_gyr[3];

static esp_err_t wr8(uint8_t reg, uint8_t v) { uint8_t b[2] = {reg, v}; return i2c_master_transmit(s_dev, b, 2, 100); }
static esp_err_t rd(uint8_t reg, uint8_t *out, size_t n) { return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, 100); }

static int try_addr(uint8_t addr) {
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(hal_i2c(), &dc, &s_dev) != ESP_OK) return -1;
    uint8_t id = 0;
    if (rd(R_CHIP_ID, &id, 1) != ESP_OK || id != 0x24) return -1;
    return 0;
}

static int imu_init(void) {
    if (s_state) return s_state > 0;
    s_state = -1;
    if (try_addr(0x69) && try_addr(0x68)) { ESP_LOGW(TAG, "BMI270 not found"); return 0; }
    wr8(R_CMD, 0xB6); /* soft reset */
    vTaskDelay(pdMS_TO_TICKS(5));
    wr8(R_PWR_CONF, 0x00); /* advanced power save off: required before the config upload */
    vTaskDelay(pdMS_TO_TICKS(2));
    wr8(R_INIT_CTRL, 0x00);
    for (size_t idx = 0; idx < sizeof(bmi270_config_blob); idx += 128) {
        uint8_t hdr[3] = {R_INIT_ADDR_0, (uint8_t)((idx >> 1) & 0x0F), (uint8_t)(idx >> 5)};
        if (i2c_master_transmit(s_dev, hdr, 3, 100) != ESP_OK) return 0;
        uint8_t chunk[1 + 128];
        chunk[0] = R_INIT_DATA;
        memcpy(chunk + 1, bmi270_config_blob + idx, 128);
        if (i2c_master_transmit(s_dev, chunk, sizeof(chunk), 200) != ESP_OK) return 0;
    }
    wr8(R_INIT_CTRL, 0x01);
    uint8_t st = 0;
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (rd(R_INTERNAL_STATUS, &st, 1) == ESP_OK && (st & 0x0F) == 1) break;
    }
    if ((st & 0x0F) != 1) { ESP_LOGW(TAG, "BMI270 init failed (status 0x%02x)", st); return 0; }
    wr8(R_PWR_CTRL, 0x0E);  /* accel + gyro + temperature on, aux off */
    wr8(R_ACC_CONF, 0xA8);  /* 100 Hz, normal filter, +-8 g (default range) */
    wr8(R_GYR_CONF, 0xA9);  /* 200 Hz, normal filter, +-2000 dps (default range) */
    s_state = 1;
    ESP_LOGI(TAG, "BMI270 ready");
    return 1;
}

static int im_update(void) {
    if (!cpos_has_perm(CPOS_PERM_IMU) || !imu_init()) return 0;
    uint8_t b[12];
    if (rd(R_ACC_X, b, 12) != ESP_OK) return 0;
    int16_t v[6];
    for (int i = 0; i < 6; i++) v[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8));
    for (int i = 0; i < 3; i++) {
        s_acc[i] = v[i] * (8.0f / 32768.0f);
        s_gyr[i] = v[3 + i] * (2000.0f / 32768.0f);
    }
    return 1;
}
static int im_enabled(void) { return cpos_has_perm(CPOS_PERM_IMU) && imu_init(); }
static void im_acc(float *x, float *y, float *z) { *x = s_acc[0]; *y = s_acc[1]; *z = s_acc[2]; }
static void im_gyr(float *x, float *y, float *z) { *x = s_gyr[0]; *y = s_gyr[1]; *z = s_gyr[2]; }

void cpos_imu_fill(cpos_imu_api_t *i) { i->update = im_update; i->enabled = im_enabled; i->accel = im_acc; i->gyro = im_gyr; }
