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
/*
 * Emulator self test: run every app once, dumping a frame while it runs, then drive the launcher
 * screens with scripted keys. Output goes to the log (FRAME blocks) for offline inspection.
 */
#include "esp_timer.h"
static int64_t s_start_us;
static char s_name[40];
static volatile int s_dumped;

static void kill_cb(void *arg) {
    if (g_app && esp_timer_get_time() - s_start_us > 2200000) g_app->exit_req = 1;
}

static void frame_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (g_app && !s_dumped && esp_timer_get_time() - s_start_us > 1600000) {
            s_dumped = 1;
            hal_display_dump(s_name);
        }
    }
}

static void ui_task(void *arg) { cpos_ui_run(); }

static void selftest(void) {
    xTaskCreate(frame_task, "frames", 4096, NULL, 2, NULL);
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
        esp_timer_start_periodic(t, 500000);
        const char *err = NULL;
        strlcpy(s_name, e->d_name, sizeof(s_name));
        s_dumped = 0;
        s_start_us = esp_timer_get_time();
        int rc = cpos_run_app(path, &err);
        esp_timer_stop(t);
        esp_timer_delete(t);
        ESP_LOGI(TAG, "SELFTEST %s -> rc=%d err=%s heap=%u", e->d_name, rc, err ? err : "-", (unsigned)esp_get_free_heap_size());
        if (rc < 0) bad++; else ok++;
    }
    if (d) closedir(d);
    ESP_LOGI(TAG, "SELFTEST APPS ok=%d bad=%d", ok, bad);

    /* launcher and system screens */
    xTaskCreate(ui_task, "ui", 8192, NULL, 4, NULL);
    vTaskDelay(pdMS_TO_TICKS(2500));
    hal_display_dump("ui-launcher");
    hal_kbd_inject(';');                 /* wraps to the last entry: System */
    hal_kbd_inject('\n');
    vTaskDelay(pdMS_TO_TICKS(1500));
    hal_display_dump("ui-system");
    for (int i = 0; i < 5; i++) hal_kbd_inject('.');
    hal_kbd_inject('\n');                /* Display setup */
    vTaskDelay(pdMS_TO_TICKS(1500));
    hal_display_dump("ui-display");
    ESP_LOGI(TAG, "SELFTEST DONE");
}
#endif

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    cpos_hal_init();
    hal_display_init();
    gfx_init();
    hal_kbd_init();
    int sd = hal_sd_mount();
    ESP_LOGI(TAG, "CardputerOS boot, sd=%d, free heap %u", sd, (unsigned)esp_get_free_heap_size());
#ifdef CONFIG_CPOS_SELFTEST
    selftest();
#else
    cpos_ui_run();
#endif
}
