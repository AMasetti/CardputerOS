/*
 * Extra tasks and mutexes for apps. Everything is recorded in the app context so it can be
 * put back when the app ends: tasks leave on their own at the next OS call once the app is
 * exiting (cpos_check_exit), and whatever is still running is deleted by the runner.
 */
#include "app_ctx.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rtos";

typedef struct { void (*fn)(void *); void *arg; int slot; } start_t;
typedef struct { SemaphoreHandle_t h; int recursive; } mtx_t;

static void trampoline(void *p) {
    start_t st = *(start_t *)p;
    free(p);
    st.fn(st.arg);
    if (g_app) g_app->xtask[st.slot] = NULL;
    vTaskDelete(NULL);
}

/* called from cpos_check_exit: an extra task of an exiting app ends here, at a point where it
 * holds no OS lock */
void cpos_rtos_exit_point(void) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < APP_MAX_TASKS; i++)
        if (g_app->xtask[i] == me) {
            g_app->xtask[i] = NULL;
            vTaskDelete(NULL);
        }
}

static int r_task_create(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio) {
    if (!g_app || !fn) return -1;
    int slot = -1;
    for (int i = 0; i < APP_MAX_TASKS; i++) if (!g_app->xtask[i]) { slot = i; break; }
    if (slot < 0) return -1;
    start_t *st = malloc(sizeof(*st));
    if (!st) return -1;
    *st = (start_t){fn, arg, slot};
    if (stack < 2048) stack = 2048;
    if (stack > 16384) stack = 16384;
    if (prio < 1) prio = 1;
    if (prio > 5) prio = 5; /* never above the app's own task */
    g_app->xtask[slot] = (TaskHandle_t)1; /* reserved until the handle is known */
    if (xTaskCreatePinnedToCore(trampoline, name ? name : "apptask", stack, st, prio, &g_app->xtask[slot], 1) != pdPASS) {
        g_app->xtask[slot] = NULL;
        free(st);
        return -1;
    }
    return slot;
}

static void r_task_delete(int id) {
    if (!g_app) return;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (id < 0) {
        for (int i = 0; i < APP_MAX_TASKS; i++) if (g_app->xtask[i] == me) g_app->xtask[i] = NULL;
        if (me != g_app->task) vTaskDelete(NULL);
        return;
    }
    if (id >= APP_MAX_TASKS || !g_app->xtask[id]) return;
    TaskHandle_t h = g_app->xtask[id];
    g_app->xtask[id] = NULL;
    if (h == me) vTaskDelete(NULL);
    vTaskDelete(h);
}

static void *r_mutex_create(int recursive) {
    if (!g_app) return NULL;
    int slot = -1;
    for (int i = 0; i < APP_MAX_MUTEX; i++) if (!g_app->mutex[i]) { slot = i; break; }
    if (slot < 0) return NULL;
    mtx_t *m = malloc(sizeof(*m));
    if (!m) return NULL;
    m->recursive = recursive;
    m->h = recursive ? xSemaphoreCreateRecursiveMutex() : xSemaphoreCreateMutex();
    if (!m->h) { free(m); return NULL; }
    g_app->mutex[slot] = m;
    return m;
}

/* waits in slices so an exit request still gets through */
static int r_mutex_lock(void *p, uint32_t timeout_ms) {
    mtx_t *m = p;
    if (!m) return -1;
    for (uint32_t t = 0;; t += 50) {
        cpos_check_exit();
        BaseType_t ok = m->recursive ? xSemaphoreTakeRecursive(m->h, pdMS_TO_TICKS(50)) : xSemaphoreTake(m->h, pdMS_TO_TICKS(50));
        if (ok == pdTRUE) return 0;
        if (timeout_ms != 0xFFFFFFFFu && t >= timeout_ms) return -1;
    }
}

static void r_mutex_unlock(void *p) {
    mtx_t *m = p;
    if (!m) return;
    if (m->recursive) xSemaphoreGiveRecursive(m->h);
    else xSemaphoreGive(m->h);
}

static void r_mutex_delete(void *p) {
    if (!p || !g_app) return;
    for (int i = 0; i < APP_MAX_MUTEX; i++) if (g_app->mutex[i] == p) g_app->mutex[i] = NULL;
    vSemaphoreDelete(((mtx_t *)p)->h);
    free(p);
}

void cpos_rtos_fill(cpos_rtos_api_t *a) {
    a->task_create = r_task_create;
    a->task_delete = r_task_delete;
    a->mutex_create = r_mutex_create;
    a->mutex_lock = r_mutex_lock;
    a->mutex_unlock = r_mutex_unlock;
    a->mutex_delete = r_mutex_delete;
}

/* the app's main task has ended: give its extra tasks a moment to leave, then remove them */
void cpos_app_stop_tasks(app_ctx_t *a) {
    a->exit_req = 1;
    for (int t = 0; t < 40; t++) {
        int left = 0;
        for (int i = 0; i < APP_MAX_TASKS; i++) if (a->xtask[i]) left++;
        if (!left) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < APP_MAX_TASKS; i++)
        if (a->xtask[i]) {
            ESP_LOGW(TAG, "deleting a task that did not stop");
            vTaskDelete(a->xtask[i]);
            a->xtask[i] = NULL;
        }
}

void cpos_app_cleanup_rtos(app_ctx_t *a) {
    for (int i = 0; i < APP_MAX_MUTEX; i++)
        if (a->mutex[i]) {
            vSemaphoreDelete(((mtx_t *)a->mutex[i])->h);
            free(a->mutex[i]);
            a->mutex[i] = NULL;
        }
}
