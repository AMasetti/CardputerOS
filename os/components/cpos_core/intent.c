/* Intents: the OS starts an app to do one job and gets a value back (docs/intents.md). */
#include "app_ctx.h"
#include "esp_timer.h"
#include <string.h>

static esp_timer_handle_t s_timer;
static struct {
    int active;                         /* an intent is being served by the running app */
    char name[CPOS_INTENT_NAME_MAX];
    char args[CPOS_INTENT_ARGS_MAX];
    int replied, status;
    char value[CPOS_INTENT_VALUE_MAX];
} s_in;

static int i_request(char *name, size_t name_max, char *args, size_t args_max) {
    if (!s_in.active) return 0;
    strlcpy(name, s_in.name, name_max);
    strlcpy(args, s_in.args, args_max);
    return 1;
}

static void i_reply(int status, const char *value) {
    s_in.status = status;
    strlcpy(s_in.value, value ? value : "", sizeof(s_in.value));
    s_in.replied = 1;
    if (s_timer) esp_timer_stop(s_timer); /* answered: a launch intent may keep running without a deadline */
}

void cpos_intent_fill(cpos_intent_api_t *a) {
    a->request = i_request;
    a->reply = i_reply;
}

static void timeout_cb(void *arg) {
    if (g_app) g_app->exit_req = 1; /* the app leaves at its next call into the OS */
}

int cpos_run_intent(const char *path, const char *name, const char *args, char *value, size_t vmax, uint32_t timeout_ms) {
    memset(&s_in, 0, sizeof(s_in));
    strlcpy(s_in.name, name, sizeof(s_in.name));
    strlcpy(s_in.args, args ? args : "", sizeof(s_in.args));
    s_in.active = 1;
    esp_timer_handle_t t = NULL;
    esp_timer_create_args_t ta = {.callback = timeout_cb, .name = "intent"};
    if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, (int64_t)(timeout_ms ? timeout_ms : 2000) * 1000);
    s_timer = t;
    const char *err = NULL;
    int rc = cpos_run_app(path, &err);
    s_timer = NULL;
    if (t) { esp_timer_stop(t); esp_timer_delete(t); }
    s_in.active = 0;
    int st = rc < 0 ? CPOS_INTENT_FAILED : (s_in.replied ? s_in.status : CPOS_INTENT_TIMEOUT);
    strlcpy(value, s_in.replied ? s_in.value : "", vmax);
    return st;
}
