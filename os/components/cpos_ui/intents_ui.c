/*
 * Intents screen (System > Intents): lists the intents the installed apps declare, asks for the
 * arguments and runs one, showing the result. It is also the way to try them by hand until
 * shortcuts exist (docs/intents.md). The os.* intents are answered here, without starting an app.
 */
#include "ui.h"
#include "app_ctx.h"
#include "hal.h"
#include "loader.h"
#include "esp_timer.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define C_ACC gfx_rgb(64, 228, 255)
#define C_OK gfx_rgb(80, 220, 120)

typedef struct { char name[32]; char app[24]; char path[96]; } entry_t;
#define MAX_ENTRIES 80
static entry_t s_e[MAX_ENTRIES];
static int s_n;

static const char *status_name(int st) {
    static const char *n[] = {"ok", "no_app", "no_perm", "timeout", "bad_arg", "failed"};
    return st >= 0 && st <= 5 ? n[st] : "?";
}

/* the object text of "name":{...} inside the compact JSON, copied to out (NUL terminated) */
static int intent_obj(const char *json, const char *name, char *out, size_t max) {
    char key[40];
    snprintf(key, sizeof(key), "\"%s\":{", name);
    const char *p = strstr(json, key);
    if (!p) return 0;
    p += strlen(key) - 1;
    int depth = 0, instr = 0;
    size_t l = 0;
    for (; *p && l < max - 1; p++) {
        if (*p == '"') instr = !instr;
        if (!instr) { if (*p == '{') depth++; else if (*p == '}') depth--; }
        out[l++] = *p;
        if (!depth) break;
    }
    out[l] = 0;
    return 1;
}

static int obj_int(const char *obj, const char *key, int def) {
    char k[24];
    snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(obj, k);
    return p ? atoi(p + strlen(k)) : def;
}

/* "r=;g=;b=" from the argument names, as a starting point for the user */
static void args_template(const char *obj, char *out, size_t max) {
    out[0] = 0;
    const char *p = strstr(obj, "\"args\":{");
    if (!p) return;
    p += 8;
    int depth = 1, instr = 0, key = 1;
    char name[24];
    int nl = 0;
    for (; *p && depth; p++) {
        if (*p == '"') {
            instr = !instr;
            if (instr) nl = 0;
            else if (key && depth == 1 && nl) {
                name[nl] = 0;
                if (out[0]) strlcat(out, ";", max);
                strlcat(out, name, max);
                strlcat(out, "=", max);
                key = 0;
            }
            continue;
        }
        if (instr) { if (nl < (int)sizeof(name) - 1) name[nl++] = *p; continue; }
        if (*p == '}') depth--;
        else if (*p == ',') key = 1;
    }
}

static void scan(void) {
    s_n = 0;
    DIR *d = opendir("/sdcard/apps");
    if (!d) return;
    static char json[3100];
    struct dirent *e;
    while ((e = readdir(d)) && s_n < MAX_ENTRIES) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".elf") || e->d_name[0] == '.') continue;
        char path[96];
        snprintf(path, sizeof(path), "/sdcard/apps/%.80s", e->d_name);
        cpos_app_header_t h;
        if (cpos_read_header(path, &h, NULL) || cpos_read_intents(path, json, sizeof(json)) < 0) continue;
        /* every top-level key: "name":{ at depth 1 */
        int depth = 0, instr = 0;
        char key[32];
        int kl = 0;
        for (const char *p = json; *p && s_n < MAX_ENTRIES; p++) {
            if (*p == '"') {
                instr = !instr;
                if (instr) kl = 0;
                else if (depth == 1 && p[1] == ':' && p[2] == '{' && kl) {
                    key[kl] = 0;
                    entry_t *en = &s_e[s_n++];
                    strlcpy(en->name, key, sizeof(en->name));
                    strlcpy(en->app, h.name, sizeof(en->app));
                    strlcpy(en->path, path, sizeof(en->path));
                }
                continue;
            }
            if (instr) { if (kl < (int)sizeof(key) - 1) key[kl++] = *p; continue; }
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
        }
    }
    closedir(d);
}

/* intents the OS answers itself */
static const char *os_intents[] = {"os.battery", "os.wifi", "os.time", "os.notify", "os.open"};

int cpos_intent_os(const char *name, const char *args, char *value, size_t vmax) {
    if (!strcmp(name, "os.battery")) { snprintf(value, vmax, "%d", hal_batt_pct()); return 0; }
    if (!strcmp(name, "os.wifi")) { snprintf(value, vmax, "%s", cpos_wifi_is_connected() ? "true" : "false"); return 0; }
    if (!strcmp(name, "os.time")) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(value, vmax, "%H:%M", &tm);
        return 0;
    }
    if (!strcmp(name, "os.notify")) {
        const char *t = strstr(args, "text=");
        if (!t) return CPOS_INTENT_BAD_ARG;
        char msg[64];
        strlcpy(msg, t + 5, sizeof(msg));
        char *sc = strchr(msg, ';');
        if (sc) *sc = 0;
        ui_status("Notification", msg);
        vTaskDelay(pdMS_TO_TICKS(1500));
        value[0] = 0;
        return 0;
    }
    return CPOS_INTENT_NO_APP;
}

static int run(const entry_t *en, const char *args, char *value, size_t vmax) {
    if (!strncmp(en->name, "os.", 3)) return cpos_intent_os(en->name, args, value, vmax);
    static char json[3100], obj[600];
    int tmo = 2000;
    if (cpos_read_intents(en->path, json, sizeof(json)) > 0 && intent_obj(json, en->name, obj, sizeof(obj))) tmo = obj_int(obj, "timeout_ms", 2000);
    ui_status(en->name, en->app);
    return ui_run_intent(en->path, en->name, args, value, vmax, tmo);
}

void intents_screen(void) {
    ui_status("Intents", "reading apps...");
    scan();
    int n = 0;
    static ui_item_t items[MAX_ENTRIES + 5];
    static const entry_t *map[MAX_ENTRIES + 5];
    static entry_t os_e[5];
    for (int i = 0; i < 5; i++) {
        strlcpy(os_e[i].name, os_intents[i], sizeof(os_e[i].name));
        strlcpy(os_e[i].app, "OS", sizeof(os_e[i].app));
        snprintf(items[n].left, 48, "%s", os_e[i].name);
        snprintf(items[n].right, 24, "OS");
        items[n].color = C_ACC;
        map[n++] = &os_e[i];
    }
    for (int i = 0; i < s_n; i++) {
        snprintf(items[n].left, 48, "%s", s_e[i].name);
        snprintf(items[n].right, 24, "%.22s", s_e[i].app);
        items[n].color = 0;
        map[n++] = &s_e[i];
    }
    int sel = 0;
    for (;;) {
        sel = ui_menu("Intents", items, n, sel, "enter: run  `: back");
        if (sel < 0) return;
        const entry_t *en = map[sel];
        char args[CPOS_INTENT_ARGS_MAX] = "";
        if (!strncmp(en->name, "os.", 3)) {
            if (!strcmp(en->name, "os.notify")) strlcpy(args, "text=", sizeof(args));
            else if (!strcmp(en->name, "os.open")) { ui_message("os.open", "Use the launcher to open apps."); continue; }
        } else {
            static char json[3100], obj[600];
            if (cpos_read_intents(en->path, json, sizeof(json)) > 0 && intent_obj(json, en->name, obj, sizeof(obj))) args_template(obj, args, sizeof(args));
        }
        if (args[0] && !ui_input(en->name, args, sizeof(args), 0)) continue;
        char value[CPOS_INTENT_VALUE_MAX];
        int st = run(en, args, value, sizeof(value));
        char m[CPOS_INTENT_VALUE_MAX + 24];
        snprintf(m, sizeof(m), "%s%s%s", status_name(st), value[0] ? ": " : "", value);
        ui_message(en->name, m);
    }
}
