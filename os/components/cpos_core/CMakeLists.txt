idf_component_register(
    SRCS "api.c" "gfx.c" "font_data.c" "loader.c" "runner.c" "net.c" "gpio_svc.c" "stubs_svc.c" "qr.c"
         "third_party/qrcodegen/qrcodegen.c"
    INCLUDE_DIRS "." "../../../sdk/include"
    REQUIRES cpos_hal esp_wifi esp_netif esp_event nvs_flash lwip esp_timer heap esp_hw_support driver
)
