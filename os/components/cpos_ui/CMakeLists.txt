idf_component_register(
    SRCS "ui.c" "ui_input.c" "store.c" "launcher.c"
    INCLUDE_DIRS "."
    REQUIRES cpos_core cpos_hal esp_http_client esp_https_ota app_update esp_partition nvs_flash mbedtls esp-tls esp_wifi
)
