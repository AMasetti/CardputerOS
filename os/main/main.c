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
#include "test_jpeg.h"
static int64_t s_start_us;
static char s_name[40];
static volatile int s_dumped;

static void kill_cb(void *arg) {
    if (g_app && esp_timer_get_time() - s_start_us > (g_app && !strncmp(g_app->img.header.name, "robot", 5) ? 9000000 : 2200000)) g_app->exit_req = 1;
}

static void frame_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (g_app && !s_dumped && esp_timer_get_time() - s_start_us > 1600000 && !(g_app && !strncmp(g_app->img.header.name, "robot", 5))) {
            s_dumped = 1;
            hal_display_dump(s_name);
        }
    }
}

static void ui_task(void *arg) { cpos_ui_run(); }

/* the Robot Control app: move down the fake robot list and try to connect */
static void script_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (g_app && !strncmp(g_app->img.header.name, "robot", 5) && esp_timer_get_time() - s_start_us > 2500000) {
            hal_display_dump("robot-scan");
            hal_kbd_inject('.');
            vTaskDelay(pdMS_TO_TICKS(1500));
            hal_display_dump("robot-scan2");
            hal_kbd_inject('\n');
            vTaskDelay(pdMS_TO_TICKS(5000));
            hal_display_dump("robot-connect");
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void watch_task(void *arg) {
    for (int i = 0; i < 3; i++) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        static char buf[1024];
        vTaskList(buf);
        printf("TASKS@%d g_app=%p exit_req=%d\n%s\n", i, (void *)g_app, g_app ? (int)g_app->exit_req : -1, buf);
    }
    vTaskDelete(NULL);
}
typedef struct { const unsigned char *d; size_t n, pos; } memsrc_t;
static size_t mem_read(void *ctx, uint8_t *buf, size_t n) {
    memsrc_t *m = ctx;
    size_t left = m->n - m->pos;
    if (n > left) n = left;
    if (buf) memcpy(buf, m->d + m->pos, n);
    m->pos += n;
    return n;
}

static void jpeg_test(void) {
    cpos_canvas_t *c = gfx_create(CPOS_SCREEN_W, CPOS_SCREEN_H, 16);
    gfx_fill(c, 0x0000);
    memsrc_t src = {test_jpeg, sizeof(test_jpeg), 0};
    int rc = cpos_api_get()->media.jpeg_draw(c, 0, 15, 160, 120, mem_read, &src);
    ESP_LOGI(TAG, "SELFTEST jpeg decode rc=%d (%u bytes)", rc, (unsigned)sizeof(test_jpeg));
    gfx_push(c, NULL, 0, 0, -1);
    hal_display_dump("jpeg-test");
    gfx_destroy(c);
}

static void selftest(void) {
    jpeg_test();
    xTaskCreate(script_task, "script", 4096, NULL, 2, NULL);
    xTaskCreate(watch_task, "watch", 4096, NULL, 2, NULL);
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
