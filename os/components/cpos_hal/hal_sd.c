/* microSD over SPI3 (shares the bus with the LoRa cap, so unused CS lines are held high). */
#include "hal.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card;

#ifdef CONFIG_CPOS_SD_FROM_FLASH
/* emulator builds: a read-only FAT partition stands in for the card */
static bool s_flash_fs;
int hal_sd_mount(void) {
    esp_vfs_fat_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 6};
    wl_handle_t unused;
    (void)unused;
    esp_err_t e = esp_vfs_fat_spiflash_mount_ro("/sdcard", "storage", &mc);
    s_flash_fs = e == ESP_OK;
    if (!s_flash_fs) ESP_LOGW(TAG, "storage mount failed: %s", esp_err_to_name(e));
    return s_flash_fs ? 0 : -1;
}
bool hal_sd_ready(void) { return s_flash_fs; }
#else
int hal_sd_mount(void) {
    if (s_card) return 0;
    spi_bus_config_t bus = {.mosi_io_num = PIN_SD_MOSI, .miso_io_num = PIN_SD_MISO, .sclk_io_num = PIN_SD_SCK,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096};
    esp_err_t e = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return -1;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 20000;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI3_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 6, .allocation_unit_size = 16 * 1024};
    e = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mc, &s_card);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(e));
        s_card = NULL;
        return -1;
    }
    ESP_LOGI(TAG, "card %s, %llu MB", s_card->cid.name, ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return 0;
}

bool hal_sd_ready(void) { return s_card != NULL; }
#endif
