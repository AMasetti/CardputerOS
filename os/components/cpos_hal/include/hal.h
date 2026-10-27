#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "cpos/abi.h"

/* Cardputer ADV pin map (single board, so these are plain constants). */
#define PIN_LCD_MOSI 35
#define PIN_LCD_SCLK 36
#define PIN_LCD_CS   37
#define PIN_LCD_DC   34
#define PIN_LCD_RST  33
#define PIN_LCD_BL   38
#define PIN_SD_SCK   40
#define PIN_SD_MISO  39
#define PIN_SD_MOSI  14
#define PIN_SD_CS    12
#define PIN_I2C_SDA  8
#define PIN_I2C_SCL  9
#define PIN_KB_INT   11
#define PIN_BAT_ADC  10
#define PIN_BTN_A    0
#define PIN_IR_TX    44

/* ST7789 135x240 panel window inside the 240x320 controller RAM, landscape. */
#define LCD_GAP_X 40
#define LCD_GAP_Y 53

void cpos_hal_init(void);                  /* GPIO preparation, shared I2C bus */
i2c_master_bus_handle_t hal_i2c(void);

void hal_display_init(void);
void hal_display_blit(int x, int y, int w, int h, const uint16_t *pix, int stride);
void hal_display_brightness(uint8_t v);

void hal_kbd_init(void);
void hal_kbd_get(cpos_kbd_t *out);
bool hal_kbd_esc(void);               /* Fn + ` held: leave the running app */

int hal_sd_mount(void);               /* 0 on success */
bool hal_sd_ready(void);

int hal_batt_mv(void);
int hal_batt_pct(void);
int hal_btn_a(void);
