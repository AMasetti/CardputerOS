/*
 * App Store: lists the GitHub tags of the app repository, installs ELF apps to the SD card and
 * updates the OS itself. Same convention as the Launcher store: a release tag "<app>-vX.Y.Z"
 * carries the asset "<app>-vX.Y.Z.elf"; the OS is the pseudo-app "os" with a ".bin" asset.
 */
#include "ui.h"
#include "app_ctx.h"
#include "hal.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_REPO "AMasetti/CardputerOS"
#define APPS_DIR "/sdcard/apps"
#define C_ACC gfx_rgb(255, 120, 0)
#define C_OK gfx_rgb(80, 220, 120)
#define C_ERR gfx_rgb(255, 90, 90)
#define C_DIM gfx_rgb(130, 135, 150)

static const char *TAG = "store";
static char s_repo[48];

const char *store_repo(void) {
    if (s_repo[0]) return s_repo;
    nvs_handle_t h;
    size_t n = sizeof(s_repo);
    if (nvs_open("os", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "repo", s_repo, &n) != ESP_OK) s_repo[0] = 0;
        nvs_close(h);
    }
    if (!s_repo[0]) strlcpy(s_repo, DEFAULT_REPO, sizeof(s_repo));
    return s_repo;
}

void store_set_repo(const char *r) {
    if (!strchr(r, '/')) return;
    strlcpy(s_repo, r, sizeof(s_repo));
    nvs_handle_t h;
    if (nvs_open("os", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "repo", s_repo);
        nvs_commit(h);
        nvs_close(h);
    }
}

int parse_version(const char *s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    int part = 0, digit = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { v[part] = v[part] * 10 + (*s - '0'); digit = 1; }
        else if (*s == '.' && digit && part < 2) { part++; digit = 0; }
        else return -1;
    }
    return (part == 2 && digit) ? 0 : -1;
}

static int vcmp(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* ---- HTTP -------------------------------------------------------------------------------- */
typedef int (*sink_t)(const uint8_t *d, size_t n, void *ctx);
typedef struct { sink_t sink; void *ctx; int fail; int64_t total; int64_t got; int status; } dl_t;

static esp_err_t http_evt(esp_http_client_event_t *e) {
    dl_t *d = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Content-Length") == 0) d->total = atoll(e->header_value);
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        int code = esp_http_client_get_status_code(e->client);
        d->status = code;
        if (code != 200) return ESP_OK;
        if (d->sink(e->data, e->data_len, d->ctx)) { d->fail = 1; return ESP_FAIL; }
        d->got += e->data_len;
    }
    return ESP_OK;
}

/* returns the HTTP status (200 ok), or <0 on transport error */
static int http_get(const char *url, sink_t sink, void *ctx, int64_t *total_out, void (*progress)(int64_t got, int64_t total)) {
    dl_t d = {.sink = sink, .ctx = ctx};
    esp_http_client_config_t cfg = {.url = url, .event_handler = http_evt, .user_data = &d, .crt_bundle_attach = esp_crt_bundle_attach,
                                    .timeout_ms = 20000, .buffer_size = 4096, .buffer_size_tx = 4096, .max_redirection_count = 6,
                                    .user_agent = "CardputerOS"};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    if (total_out) *total_out = d.total;
    esp_http_client_cleanup(c);
    (void)progress;
    if (d.fail) return -2;
    if (err != ESP_OK) { ESP_LOGW(TAG, "http %s: %s", url, esp_err_to_name(err)); return -1; }
    return status;
}

/* ---- tags --------------------------------------------------------------------------------- */
typedef struct { char name[32]; int v[3]; } tag_t;
#define MAX_TAGS 40
typedef struct { tag_t t[MAX_TAGS]; int n; int st; int mi; char cur[64]; int cl; int raw; } tags_ctx_t;

static void add_tag(tags_ctx_t *c, const char *tag) {
    c->raw++;
    const char *dash = NULL;
    for (const char *p = tag; *p; p++) if (p[0] == '-' && p[1] == 'v') dash = p;
    if (!dash || dash == tag) return;
    int v[3];
    if (parse_version(dash + 2, v)) return;
    char name[32];
    size_t l = dash - tag;
    if (l >= sizeof(name)) return;
    memcpy(name, tag, l);
    name[l] = 0;
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->t[i].name, name)) continue;
        if (vcmp(c->t[i].v, v) < 0) memcpy(c->t[i].v, v, sizeof(v));
        return;
    }
    if (c->n < MAX_TAGS) {
        strlcpy(c->t[c->n].name, name, sizeof(c->t[c->n].name));
        memcpy(c->t[c->n].v, v, sizeof(v));
        c->n++;
    }
}

/* streaming scan for  "name" : "<value>"  in the pretty-printed JSON array */
static int tags_sink(const uint8_t *d, size_t n, void *ctx) {
    tags_ctx_t *c = ctx;
    static const char pat[] = "\"name\"";
    for (size_t i = 0; i < n; i++) {
        char ch = d[i];
        switch (c->st) {
            case 0:
                if (ch == pat[c->mi]) { if (++c->mi == 6) { c->st = 1; c->mi = 0; } }
                else c->mi = (ch == pat[0]) ? 1 : 0;
                break;
            case 1:
                if (ch == '"') { c->st = 2; c->cl = 0; }
                else if (ch != ':' && ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') c->st = 0;
                break;
            case 2:
                if (ch == '"') { c->cur[c->cl] = 0; add_tag(c, c->cur); c->st = 0; }
                else if (c->cl < (int)sizeof(c->cur) - 1) c->cur[c->cl++] = ch;
                break;
        }
    }
    return 0;
}

static int fetch_tags(tags_ctx_t *c) {
    memset(c, 0, sizeof(*c));
    for (int page = 1; page <= 3; page++) {
        char url[160];
        snprintf(url, sizeof(url), "https://api.github.com/repos/%s/tags?per_page=100&page=%d", store_repo(), page);
        c->st = 0; c->mi = 0; c->raw = 0;
        int st = http_get(url, tags_sink, c, NULL, NULL);
        if (st == 404) { ui_message("Store", "Repository not found (is it private?)."); return -1; }
        if (st == 403) { ui_message("Store", "GitHub rate limit reached. Try again later."); return -1; }
        if (st != 200) { ui_message("Store", "Could not reach GitHub. Check the WiFi."); return -1; }
        if (c->raw < 100) break; /* last page */
    }
    return 0;
}

/* ---- installed apps ----------------------------------------------------------------------- */
typedef struct { char name[24]; int v[3]; char file[64]; } inst_t;

static int list_installed(inst_t *out, int max) {
    int n = 0;
    DIR *d = opendir(APPS_DIR);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".elf") != 0 || e->d_name[0] == '.') continue;
        char p[96];
        snprintf(p, sizeof(p), APPS_DIR "/%s", e->d_name);
        cpos_app_header_t h;
        if (cpos_read_header(p, &h, NULL)) continue;
        strlcpy(out[n].name, h.name, sizeof(out[n].name));
        parse_version(h.version, out[n].v);
        strlcpy(out[n].file, e->d_name, sizeof(out[n].file));
        n++;
    }
    closedir(d);
    return n;
}

/* ---- download sinks ----------------------------------------------------------------------- */
static int file_sink(const uint8_t *d, size_t n, void *ctx) { return fwrite(d, 1, n, (FILE *)ctx) == n ? 0 : -1; }

typedef struct { esp_ota_handle_t h; int err; } ota_ctx_t;
static int ota_sink(const uint8_t *d, size_t n, void *ctx) {
    ota_ctx_t *o = ctx;
    return esp_ota_write(o->h, d, n) == ESP_OK ? 0 : -1;
}

static const char *perm_names[] = {"SD card", "WiFi", "Bluetooth", "Speaker", "Microphone", "Motion sensor", "Infrared", "GPIO pins"};

static void perms_text(uint32_t p, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < 8; i++) {
        if (!(p & (1u << i))) continue;
        if (out[0]) strlcat(out, ", ", n);
        strlcat(out, perm_names[i], n);
    }
    if (!out[0]) strlcpy(out, "none", n);
}

static int install_app(const char *name, const int v[3]) {
    char tag[64], url[200], tmp[] = APPS_DIR "/.download";
    snprintf(tag, sizeof(tag), "%s-v%d.%d.%d", name, v[0], v[1], v[2]);
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/%s.elf", store_repo(), tag, tag);
    mkdir(APPS_DIR, 0777);
    FILE *f = fopen(tmp, "wb");
    if (!f) { ui_message("Install", "Cannot write to the SD card."); return -1; }
    ui_status("Downloading...", tag);
    int st = http_get(url, file_sink, f, NULL, NULL);
    fclose(f);
    if (st != 200) {
        unlink(tmp);
        ui_message("Install", st == 404 ? "This version has no .elf file attached." : "Download failed.");
        return -1;
    }
    cpos_app_header_t h;
    const char *err = NULL;
    if (cpos_read_header(tmp, &h, &err)) {
        unlink(tmp);
        char m[80];
        snprintf(m, sizeof(m), "Invalid app file: %s", err ? err : "?");
        ui_message("Install", m);
        return -1;
    }
    if (h.abi_major != CPOS_ABI_MAJOR || h.abi_minor > CPOS_ABI_MINOR) {
        unlink(tmp);
        ui_message("Install", "This app needs a different OS version. Update the OS first.");
        return -1;
    }
    char pt[64], m[160];
    perms_text(h.perms, pt, sizeof(pt));
    snprintf(m, sizeof(m), "%s %s will be able to use: %s. Install?", h.name, h.version, pt);
    if (!ui_confirm("Permissions", m)) { unlink(tmp); return -1; }

    /* keep only the newest version of this app */
    static inst_t old[40];
    int n = list_installed(old, 40);
    for (int i = 0; i < n; i++) {
        if (strcmp(old[i].name, h.name)) continue;
        char p[100];
        snprintf(p, sizeof(p), APPS_DIR "/%s", old[i].file);
        unlink(p);
    }
    char dst[100];
    snprintf(dst, sizeof(dst), APPS_DIR "/%s.elf", tag);
    unlink(dst);
    if (rename(tmp, dst)) { ui_message("Install", "Could not save the app."); return -1; }
    ui_message("Install", "Installed. Find it in the launcher.");
    return 0;
}

static int update_os(const int v[3]) {
    char tag[32], url[200];
    snprintf(tag, sizeof(tag), "os-v%d.%d.%d", v[0], v[1], v[2]);
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/%s.bin", store_repo(), tag, tag);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { ui_message("OS update", "No update partition."); return -1; }
    ota_ctx_t o = {0};
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &o.h) != ESP_OK) { ui_message("OS update", "Cannot start the update."); return -1; }
    ui_status("Updating OS...", tag);
    int st = http_get(url, ota_sink, &o, NULL, NULL);
    if (st != 200) {
        esp_ota_abort(o.h);
        ui_message("OS update", st == 404 ? "This release has no .bin file." : "Download failed.");
        return -1;
    }
    if (esp_ota_end(o.h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        ui_message("OS update", "The downloaded image is not valid.");
        return -1;
    }
    ui_status("Done", "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return 0;
}

void store_screen(void) {
    if (!hal_sd_ready()) { ui_message("Store", "Insert an SD card first."); return; }
    if (!cpos_wifi_is_connected()) {
        ui_status("Connecting...", "using the saved WiFi");
        if (cpos_wifi_autoconnect(12000) != 0) {
            ui_message("Store", "No WiFi. Set it up in the WiFi menu first.");
            return;
        }
    }
    ui_status("Checking updates...", store_repo());
    static tags_ctx_t tags;
    if (fetch_tags(&tags)) return;
    if (tags.n == 0) { ui_message("Store", "No releases found in this repository."); return; }

    int sel = 0;
    for (;;) {
        static inst_t inst[40];
        int ni = list_installed(inst, 40);
        static ui_item_t items[MAX_TAGS];
        int idx[MAX_TAGS], n = 0;
        const esp_app_desc_t *d = esp_app_get_description();
        int osv[3];
        parse_version(d->version, osv);
        for (int i = 0; i < tags.n; i++) {
            ui_item_t *it = &items[n];
            const int *lv = tags.t[i].v;
            int is_os = strcmp(tags.t[i].name, "os") == 0;
            int cur[3] = {-1, 0, 0}, have = 0;
            if (is_os) { memcpy(cur, osv, sizeof(cur)); have = 1; }
            else for (int j = 0; j < ni; j++) if (!strcmp(inst[j].name, tags.t[i].name)) { memcpy(cur, inst[j].v, sizeof(cur)); have = 1; }
            snprintf(it->left, sizeof(it->left), "%s%s", is_os ? "OS: " : "", is_os ? "CardputerOS" : tags.t[i].name);
            if (!have) { snprintf(it->right, sizeof(it->right), "get v%d.%d.%d", lv[0], lv[1], lv[2]); it->color = 0; }
            else if (vcmp(cur, lv) < 0) { snprintf(it->right, sizeof(it->right), "update v%d.%d.%d", lv[0], lv[1], lv[2]); it->color = C_ACC; }
            else { snprintf(it->right, sizeof(it->right), "v%d.%d.%d ok", cur[0], cur[1], cur[2]); it->color = C_OK; }
            idx[n++] = i;
        }
        sel = ui_menu("App Store", items, n, sel, "enter: install/update  `: back");
        if (sel < 0) return;
        tag_t *t = &tags.t[idx[sel]];
        if (strcmp(t->name, "os") == 0) {
            if (vcmp(osv, t->v) >= 0) { ui_message("OS update", "The OS is up to date."); continue; }
            char m[96];
            snprintf(m, sizeof(m), "Update CardputerOS to v%d.%d.%d and restart?", t->v[0], t->v[1], t->v[2]);
            if (ui_confirm("OS update", m)) update_os(t->v);
        } else {
            install_app(t->name, t->v);
        }
    }
}
