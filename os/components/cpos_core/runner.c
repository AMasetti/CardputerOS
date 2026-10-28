/* Runs one app at a time in its own task, and puts everything back when it ends. */
#include "app_ctx.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "hal.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "runner";

#define CRASH_MAGIC 0xC5A5C0DEu
RTC_NOINIT_ATTR static uint32_t s_crash_magic;
RTC_NOINIT_ATTR static char s_crash_app[24];

static void app_task(void *arg) {
    app_ctx_t *a = arg;
    if (setjmp(a->jb) == 0) a->result = a->img.entry(cpos_api_get());
    a->done = 1;
    xTaskNotifyGive(a->waiter);
    vTaskDelete(NULL);
}

void cpos_boot_check(char *msg, size_t n) {
    msg[0] = 0;
    esp_reset_reason_t r = esp_reset_reason();
    bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (crash && s_crash_magic == CRASH_MAGIC && s_crash_app[0]) {
        s_crash_app[sizeof(s_crash_app) - 1] = 0;
        const char *why = r == ESP_RST_PANIC ? "crashed" : "hung";
        snprintf(msg, n, "%s %s and the system restarted", s_crash_app, why);
    }
    s_crash_magic = 0;
    s_crash_app[0] = 0;
}

int cpos_run_app(const char *path, const char **err) {
    static const char *e0;
    if (!err) err = &e0;
    app_ctx_t *a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!a) { *err = "out of memory"; return -1; }
    if (cpos_load(path, &a->img, err)) { free(a); return -1; }
    a->perms = a->img.header.perms;
    uint32_t stack = a->img.header.stack_size;
    if (stack < 4096) stack = 8192;
    if (stack > 32768) stack = 32768;
    for (int i = 0; i < 8; i++) a->pwm_pin[i] = -1;
    for (int i = 0; i < APP_MAX_FDS; i++) a->fds[i] = -1;
    a->waiter = xTaskGetCurrentTaskHandle();

    s_crash_magic = CRASH_MAGIC;
    strlcpy(s_crash_app, a->img.header.name, sizeof(s_crash_app));
    g_app = a;
    ESP_LOGI(TAG, "start %s (stack %u, free heap %u, largest block %u)", a->img.header.name, (unsigned)stack, (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (xTaskCreatePinnedToCore(app_task, "app", stack, a, 5, &a->task, 1) != pdPASS) {
        g_app = NULL;
        cpos_unload(&a->img);
        free(a);
        *err = "cannot start task";
        return -1;
    }

    TickType_t req_at = 0;
    while (!a->done) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30));
        if (a->done) break;
        if (!a->exit_req && hal_kbd_esc()) a->exit_req = 1;
        if (a->exit_req && !req_at) req_at = xTaskGetTickCount();
        if (a->exit_req && xTaskGetTickCount() - req_at > pdMS_TO_TICKS(2000)) {
            ESP_LOGE(TAG, "app did not stop, restarting");
            esp_restart();
        }
    }
    int res = a->result;
    ESP_LOGI(TAG, "%s ended (%d)", a->img.header.name, res);
    cpos_app_release(a);
    cpos_unload(&a->img);
    g_app = NULL;
    s_crash_magic = 0;
    free(a);
    return res;
}
