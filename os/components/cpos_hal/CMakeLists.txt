idf_component_register(
    SRCS "hal_display.c" "hal_input.c" "hal_sd.c" "hal_power.c"
    INCLUDE_DIRS "include" "../../../sdk/include"
    REQUIRES driver esp_lcd esp_adc fatfs sdmmc esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_ledc
    PRIV_REQUIRES esp_timer mbedtls
)
