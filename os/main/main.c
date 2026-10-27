#include "app_ctx.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "hal.h"
#include "nvs_flash.h"
#include "ui.h"
#include <dirent.h>
#include <string.h>
#include "sdkconfig.h"

static const char *TAG = "cpos";

#ifdef CONFIG_CPOS_SELFTEST
#include "esp_timer.h"
static void kill_cb(void *arg) { if (g_app) g_app->exit_req = 1; }
static void selftest(void) {
    DIR *d = opendir("/sdcard/apps");
    struct dirent *e;
    int ok = 0, bad = 0;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".elf")) continue;
        char path[128];
        snprintf(path, sizeof(path), "/sdcard/apps/%s", e->d_name);
        esp_timer_handle_t t;
        esp_timer_create_args_t a = {.callback = kill_cb, .name = "kill"};
        esp_timer_create(&a, &t);
        esp_timer_start_once(t, 3000000);
        const char *err = NULL;
        int rc = cpos_run_app(path, &err);
        esp_timer_stop(t);
        esp_timer_delete(t);
        ESP_LOGI(TAG, "SELFTEST %s -> rc=%d err=%s heap=%u", e->d_name, rc, err ? err : "-", (unsigned)esp_get_free_heap_size());
        if (rc < 0) bad++; else ok++;
    }
    if (d) closedir(d);
    ESP_LOGI(TAG, "SELFTEST DONE ok=%d bad=%d", ok, bad);
}
#endif

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    cpos_hal_init();
#ifndef CONFIG_CPOS_SELFTEST
    hal_display_init();
    hal_kbd_init();
#endif
    gfx_init();
    int sd = hal_sd_mount();
    ESP_LOGI(TAG, "CardputerOS boot, sd=%d, free heap %u", sd, (unsigned)esp_get_free_heap_size());
#ifdef CONFIG_CPOS_SELFTEST
    selftest();
    return;
#else
    /* bring the saved WiFi up in the background so the store opens instantly */
    cpos_ui_run();
#endif
}
