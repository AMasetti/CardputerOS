idf_component_register(
    SRCS "api.c" "gfx.c" "font_data.c" "loader.c" "runner.c" "net.c" "gpio_svc.c" "imu.c" "audio.c" "ble.c" "rtos.c" "media.c"
         "third_party/libhelix-mp3/bitstream.c" "third_party/libhelix-mp3/buffers.c" "third_party/libhelix-mp3/dct32.c" "third_party/libhelix-mp3/dequant.c" "third_party/libhelix-mp3/dqchan.c" "third_party/libhelix-mp3/huffman.c" "third_party/libhelix-mp3/hufftabs.c" "third_party/libhelix-mp3/imdct.c" "third_party/libhelix-mp3/mp3dec.c" "third_party/libhelix-mp3/mp3tabs.c" "third_party/libhelix-mp3/polyphase.c" "third_party/libhelix-mp3/scalfact.c" "third_party/libhelix-mp3/stproc.c" "third_party/libhelix-mp3/subband.c" "third_party/libhelix-mp3/trigtabs.c" "third_party/bmi270/bmi270_config.c" "qr.c"
         "third_party/qrcodegen/qrcodegen.c"
    INCLUDE_DIRS "." "third_party/libhelix-mp3" "../../../sdk/include"
    REQUIRES bt mbedtls cpos_hal esp_driver_i2s esp_driver_i2c esp_wifi esp_netif esp_event nvs_flash lwip esp_timer heap esp_hw_support driver
)
