/* The ABI table: system services, memory/file tracking, and the libc forwarding table. */
#include "app_ctx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "hal.h"
#include "nvs.h"
#include "rom/ets_sys.h"
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "api";
app_ctx_t *g_app;

int cpos_has_perm(uint32_t p) { return g_app && (g_app->perms & p) == p; }

void cpos_check_exit(void) {
    if (!g_app || !g_app->exit_req) return;
    if (xTaskGetCurrentTaskHandle() == g_app->task) longjmp(g_app->jb, 1);
    cpos_rtos_exit_point(); /* an extra app task ends here */
}

/* ---- time ------------------------------------------------------------------ */
static uint32_t a_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t a_micros(void) { return (uint32_t)esp_timer_get_time(); }
static int64_t a_timer_us(void) { return esp_timer_get_time(); }
static void a_delay_ms(uint32_t ms) {
    cpos_check_exit();
    while (ms > 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
        ms -= 20;
        cpos_check_exit();
    }
    if (ms) vTaskDelay(ms >= 10 ? pdMS_TO_TICKS(ms) : 1);
}
static void a_delay_us(uint32_t us) {
    if (us >= 2000) a_delay_ms(us / 1000);
    else ets_delay_us(us);
}
static uint32_t a_random(void) { return esp_random(); }
static void a_log(const char *s) { fputs(s, stdout); }
static size_t a_free_heap(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static int a_exit_requested(void) { return g_app && g_app->exit_req; }
static void a_exit_app(int code) {
    if (g_app) {
        g_app->result = code;
        longjmp(g_app->jb, 1);
    }
}

static void a_kbd_state(cpos_kbd_t *o) {
    cpos_check_exit();
    hal_kbd_get(o);
}

/* ---- tracked memory -------------------------------------------------------------- */
#define HDR ((size_t)sizeof(alloc_hdr_t))
static void *a_malloc(size_t n) {
    if (!g_app || !n) return NULL;
    alloc_hdr_t *h = heap_caps_malloc(n + HDR, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!h && gfx_drop_spare()) h = heap_caps_malloc(n + HDR, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!h) return NULL;
    h->size = n;
    h->prev = NULL;
    h->next = g_app->allocs;
    if (h->next) h->next->prev = h;
    g_app->allocs = h;
    return (uint8_t *)h + HDR;
}
static void a_free(void *p) {
    if (!p || !g_app) return;
    alloc_hdr_t *h = (alloc_hdr_t *)((uint8_t *)p - HDR);
    if (h->prev) h->prev->next = h->next;
    else g_app->allocs = h->next;
    if (h->next) h->next->prev = h->prev;
    heap_caps_free(h);
}
static void *a_calloc(size_t n, size_t sz) {
    void *p = a_malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}
static void *a_realloc(void *p, size_t n) {
    if (!p) return a_malloc(n);
    if (!n) { a_free(p); return NULL; }
    alloc_hdr_t *h = (alloc_hdr_t *)((uint8_t *)p - HDR);
    void *q = a_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->size < n ? h->size : n);
    a_free(p);
    return q;
}
char *cpos_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = a_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
void cpos_abort(void) { a_exit_app(-1); }
int cpos_vprintf(const char *f, va_list ap) {
    char buf[200];
    int n = vsnprintf(buf, sizeof(buf), f, ap);
    a_log(buf);
    return n;
}

void cpos_app_free_all(app_ctx_t *a) {
    alloc_hdr_t *h = a->allocs;
    while (h) {
        alloc_hdr_t *nx = h->next;
        heap_caps_free(h);
        h = nx;
    }
    a->allocs = NULL;
}

/* ---- SD files and directories ---------------------------------------------------- */
static int sd_path(const char *p) { return p && strncmp(p, "/sdcard", 7) == 0; }

FILE *cpos_fopen(const char *p, const char *m) {
    if (!g_app || !(g_app->perms & CPOS_PERM_SD) || !sd_path(p)) return NULL;
    for (int i = 0; i < APP_MAX_FILES; i++) {
        if (g_app->files[i]) continue;
        FILE *f = fopen(p, m);
        g_app->files[i] = f;
        return f;
    }
    return NULL;
}
int cpos_fclose(FILE *f) {
    if (g_app)
        for (int i = 0; i < APP_MAX_FILES; i++)
            if (g_app->files[i] == f) g_app->files[i] = NULL;
    return fclose(f);
}
DIR *cpos_opendir(const char *p) {
    if (!g_app || !(g_app->perms & CPOS_PERM_SD) || !sd_path(p)) return NULL;
    for (int i = 0; i < APP_MAX_DIRS; i++) {
        if (g_app->dirs[i]) continue;
        g_app->dirs[i] = opendir(p);
        return g_app->dirs[i];
    }
    return NULL;
}
int cpos_closedir(DIR *d) {
    if (g_app)
        for (int i = 0; i < APP_MAX_DIRS; i++)
            if (g_app->dirs[i] == d) g_app->dirs[i] = NULL;
    return closedir(d);
}
static void app_close_files(app_ctx_t *a) {
    for (int i = 0; i < APP_MAX_FILES; i++)
        if (a->files[i]) { fclose(a->files[i]); a->files[i] = NULL; }
    for (int i = 0; i < APP_MAX_DIRS; i++)
        if (a->dirs[i]) { closedir(a->dirs[i]); a->dirs[i] = NULL; }
}

/* ---- canvases made by the app are released with it ------------------------------ */
static cpos_canvas_t *app_gfx_create(int w, int h, int d) {
    cpos_check_exit();
    cpos_canvas_t *c = gfx_create(w, h, d);
    if (!c) { /* an app that cannot get its canvas cannot draw: stop it cleanly instead of crashing */
        char m[80];
        snprintf(m, sizeof(m), "app: cannot create a %dx%d canvas (free %u)\n", w, h, (unsigned)a_free_heap());
        a_log(m);

        a_exit_app(-5);
    }
    if (c && g_app)
        for (int i = 0; i < APP_MAX_CANVAS; i++)
            if (!g_app->canvases[i]) { g_app->canvases[i] = c; break; }
    return c;
}
static void app_gfx_destroy(cpos_canvas_t *c) {
    if (g_app)
        for (int i = 0; i < APP_MAX_CANVAS; i++)
            if (g_app->canvases[i] == c) g_app->canvases[i] = NULL;
    gfx_destroy(c);
}
static void app_gfx_push(cpos_canvas_t *s, cpos_canvas_t *d, int x, int y, int t) {
    cpos_check_exit();
    gfx_push(s, d, x, y, t);
}
static void app_close_canvases(app_ctx_t *a) {
    for (int i = 0; i < APP_MAX_CANVAS; i++)
        if (a->canvases[i]) { gfx_destroy(a->canvases[i]); a->canvases[i] = NULL; }
}

/* ---- preferences (NVS, one namespace per app) ------------------------------------ */
static nvs_handle_t s_nvs[8];
static int s_nvs_used[8];

static void ns_name(char *out, const char *ns) {
    uint32_t h = 2166136261u;
    for (const char *p = ns; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    snprintf(out, 16, "a%.6s_%04x", g_app ? g_app->img.header.name : "x", (unsigned)(h & 0xFFFF));
}
static void key_name(char *out, const char *key) {
    uint32_t h = 2166136261u;
    for (const char *p = key; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    snprintf(out, 16, "%08x", (unsigned)h);
}
static int p_open(const char *ns, int ro) {
    char name[16];
    ns_name(name, ns);
    for (int i = 0; i < 8; i++) {
        if (s_nvs_used[i]) continue;
        if (nvs_open(name, ro ? NVS_READONLY : NVS_READWRITE, &s_nvs[i]) != ESP_OK) return -1;
        s_nvs_used[i] = 1;
        return i;
    }
    return -1;
}
static void p_close(int h) {
    if (h < 0 || h >= 8 || !s_nvs_used[h]) return;
    nvs_commit(s_nvs[h]);
    nvs_close(s_nvs[h]);
    s_nvs_used[h] = 0;
}
static int p_get(int h, const char *key, int type, void *out, size_t *len) {
    if (h < 0 || h >= 8 || !s_nvs_used[h]) return -1;
    char k[16];
    key_name(k, key);
    size_t n = *len;
    if (n == 0) out = NULL; /* size query */
    esp_err_t e = nvs_get_blob(s_nvs[h], k, out, &n);
    if (e == ESP_OK) { *len = n; return 0; }
    if (e == ESP_ERR_NVS_INVALID_LENGTH) { *len = n; return -2; }
    return -1;
}
static int p_set(int h, const char *key, int type, const void *val, size_t len) {
    if (h < 0 || h >= 8 || !s_nvs_used[h]) return -1;
    char k[16];
    key_name(k, key);
    return nvs_set_blob(s_nvs[h], k, val, len) == ESP_OK ? 0 : -1;
}
static int p_remove(int h, const char *key) {
    if (h < 0 || h >= 8 || !s_nvs_used[h]) return -1;
    char k[16];
    key_name(k, key);
    return nvs_erase_key(s_nvs[h], k) == ESP_OK ? 0 : -1;
}
static int p_clear(int h) {
    if (h < 0 || h >= 8 || !s_nvs_used[h]) return -1;
    return nvs_erase_all(s_nvs[h]) == ESP_OK ? 0 : -1;
}
void cpos_app_cleanup_prefs(app_ctx_t *a) {
    for (int i = 0; i < 8; i++) p_close(i);
}

/* ---- power ----------------------------------------------------------------------- */
static int a_batt_level(void) { return hal_batt_pct(); }
static int a_batt_mv(void) { return hal_batt_mv(); }
static int a_btn_a(void) { return hal_btn_a(); }

/* ---- the table -------------------------------------------------------------------- */
const cpos_api_t g_api = {
    .abi_major = CPOS_ABI_MAJOR,
    .abi_minor = CPOS_ABI_MINOR,
    .size = sizeof(cpos_api_t),
    .millis = a_millis,
    .micros = a_micros,
    .timer_us = a_timer_us,
    .delay_ms = a_delay_ms,
    .delay_us = a_delay_us,
    .random32 = a_random,
    .log = a_log,
    .malloc = a_malloc,
    .calloc = a_calloc,
    .realloc = a_realloc,
    .free = a_free,
    .free_heap = a_free_heap,
    .exit_requested = a_exit_requested,
    .exit_app = a_exit_app,
    .kbd_state = a_kbd_state,
    .power = {a_batt_level, a_batt_mv, a_btn_a},
    .prefs = {p_open, p_close, p_get, p_set, p_remove, p_clear},
    .libc = {
#include "libc_fill.inc"
    },
};

/* api tables that need a runtime fill are completed once, before any app runs */
static cpos_api_t s_api_rw;
const cpos_api_t *cpos_api_get(void) {
    if (!s_api_rw.size) {
        memcpy(&s_api_rw, &g_api, sizeof(g_api));
        gfx_fill_api(&s_api_rw.gfx);
        s_api_rw.gfx.create = app_gfx_create;
        s_api_rw.gfx.destroy = app_gfx_destroy;
        s_api_rw.gfx.push = app_gfx_push;
        cpos_net_fill(&s_api_rw.net);
        cpos_audio_fill(&s_api_rw.audio);
        cpos_imu_fill(&s_api_rw.imu);
        cpos_ble_fill(&s_api_rw.ble, &s_api_rw.crypto);
        cpos_rtos_fill(&s_api_rw.rtos);
        cpos_media_fill(&s_api_rw.media);
        extern void cpos_gpio_fill(cpos_gpio_api_t *);
        cpos_gpio_fill(&s_api_rw.gpio);
    }
    return &s_api_rw;
}

void cpos_app_release(app_ctx_t *a) {
    app_close_files(a);
    app_close_canvases(a);
    gfx_screen_release();
    cpos_app_cleanup_net(a);
    cpos_app_cleanup_audio(a);
    cpos_app_cleanup_ble(a);
    cpos_app_cleanup_media(a);
    cpos_app_cleanup_rtos(a);
    cpos_app_cleanup_gpio(a);
    cpos_app_cleanup_prefs(a);
    cpos_app_free_all(a);
}
