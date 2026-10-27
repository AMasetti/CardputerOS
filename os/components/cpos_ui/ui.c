/* Launcher and the small set of OS screens (WiFi, system). */
#include "ui.h"
#include "app_ctx.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_log.h"
#include "hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define C_BG gfx_rgb(12, 14, 22)
#define C_HDR gfx_rgb(24, 28, 44)
#define C_ACC gfx_rgb(255, 120, 0)
#define C_TXT 0xFFFF
#define C_DIM gfx_rgb(130, 135, 150)
#define C_OK gfx_rgb(80, 220, 120)
#define C_ERR gfx_rgb(255, 90, 90)
#define C_SEL gfx_rgb(40, 48, 78)

static const char *TAG = "ui";
cpos_canvas_t *ui_cv;

void ui_present(void) { gfx_push(ui_cv, NULL, 0, 0, -1); }

void ui_header(const char *title) {
    gfx_fill_rect(ui_cv, 0, 0, 240, 20, C_HDR);
    gfx_set_font(ui_cv, CPOS_FONT_JP12);
    gfx_set_text_datum(ui_cv, 4);
    gfx_set_text_color(ui_cv, C_ACC, 0, 0);
    gfx_draw_string(ui_cv, title, 6, 10);
    char r[40];
    snprintf(r, sizeof(r), "%s %d%%", cpos_wifi_is_connected() ? "wifi" : "    ", hal_batt_pct());
    gfx_set_text_datum(ui_cv, 6);
    gfx_set_text_color(ui_cv, cpos_wifi_is_connected() ? C_OK : C_DIM, 0, 0);
    gfx_draw_string(ui_cv, r, 236, 10);
}

void ui_status(const char *l1, const char *l2) {
    gfx_fill(ui_cv, C_BG);
    ui_header("CardputerOS");
    gfx_set_font(ui_cv, CPOS_FONT_DEJAVU18);
    gfx_set_text_datum(ui_cv, 5);
    gfx_set_text_color(ui_cv, C_TXT, 0, 0);
    gfx_draw_string(ui_cv, l1, 120, 62);
    if (l2) {
        gfx_set_font(ui_cv, CPOS_FONT_JP12);
        gfx_set_text_color(ui_cv, C_DIM, 0, 0);
        gfx_draw_string(ui_cv, l2, 120, 92);
    }
    ui_present();
}

void ui_progress(const char *title, const char *l2, int pct) {
    gfx_fill(ui_cv, C_BG);
    ui_header(title);
    gfx_set_font(ui_cv, CPOS_FONT_JP12);
    gfx_set_text_datum(ui_cv, 5);
    gfx_set_text_color(ui_cv, C_TXT, 0, 0);
    gfx_draw_string(ui_cv, l2, 120, 52);
    gfx_round_rect(ui_cv, 20, 70, 200, 14, 4, C_DIM, 0);
    if (pct > 0) gfx_round_rect(ui_cv, 22, 72, (196 * (pct > 100 ? 100 : pct)) / 100, 10, 3, C_ACC, 1);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", pct);
    gfx_draw_string(ui_cv, b, 120, 102);
    ui_present();
}

static void wrap_text(const char *text, int x, int y, int w, int lh) {
    char line[64];
    const char *p = text;
    while (*p) {
        int n = 0;
        int last_space = -1;
        line[0] = 0;
        while (p[n] && p[n] != '\n') {
            if (p[n] == ' ') last_space = n;
            if (n >= (int)sizeof(line) - 2) break;
            memcpy(line, p, n + 1);
            line[n + 1] = 0;
            if (gfx_text_width(ui_cv, line) > w) {
                if (last_space > 0) n = last_space;
                break;
            }
            n++;
        }
        memcpy(line, p, n);
        line[n] = 0;
        gfx_draw_string(ui_cv, line, x, y);
        y += lh;
        p += n;
        while (*p == ' ' || *p == '\n') p++;
        if (y > 125) break;
    }
}

void ui_message(const char *title, const char *text) {
    gfx_fill(ui_cv, C_BG);
    ui_header(title);
    gfx_set_font(ui_cv, CPOS_FONT_JP12);
    gfx_set_text_datum(ui_cv, 0);
    gfx_set_text_color(ui_cv, C_TXT, 0, 0);
    wrap_text(text, 8, 30, 224, 15);
    gfx_set_text_color(ui_cv, C_DIM, 0, 0);
    gfx_draw_string(ui_cv, "press any key", 8, 118);
    ui_present();
    ui_key(-1);
}

int ui_confirm(const char *title, const char *text) {
    gfx_fill(ui_cv, C_BG);
    ui_header(title);
    gfx_set_font(ui_cv, CPOS_FONT_JP12);
    gfx_set_text_datum(ui_cv, 0);
    gfx_set_text_color(ui_cv, C_TXT, 0, 0);
    wrap_text(text, 8, 28, 224, 15);
    gfx_set_text_color(ui_cv, C_DIM, 0, 0);
    gfx_draw_string(ui_cv, "enter: yes     `: no", 8, 118);
    ui_present();
    for (;;) {
        int k = ui_key(-1);
        if (k == UI_ENTER) return 1;
        if (k == UI_BACK) return 0;
    }
}

int ui_menu(const char *title, ui_item_t *items, int n, int sel, const char *hint) {
    const int rows = 6, rh = 17, top = 22;
    int first = 0;
    if (sel < 0 || sel >= n) sel = 0;
    for (;;) {
        if (sel < first) first = sel;
        if (sel >= first + rows) first = sel - rows + 1;
        gfx_fill(ui_cv, C_BG);
        ui_header(title);
        gfx_set_font(ui_cv, CPOS_FONT_JP12);
        for (int i = 0; i < rows && first + i < n; i++) {
            ui_item_t *it = &items[first + i];
            int y = top + i * rh;
            if (first + i == sel) {
                gfx_round_rect(ui_cv, 3, y, 234, rh - 1, 4, C_SEL, 1);
                gfx_fill_rect(ui_cv, 3, y + 3, 3, rh - 7, C_ACC);
            }
            gfx_set_text_datum(ui_cv, 4);
            gfx_set_text_color(ui_cv, it->color ? it->color : C_TXT, 0, 0);
            gfx_draw_string(ui_cv, it->left, 12, y + rh / 2);
            gfx_set_text_datum(ui_cv, 6);
            gfx_set_text_color(ui_cv, C_DIM, 0, 0);
            gfx_draw_string(ui_cv, it->right, 232, y + rh / 2);
        }
        if (n > rows) {
            int bh = 100 * rows / n;
            int by = top + (100 - bh) * first / (n - rows);
            gfx_fill_rect(ui_cv, 238, by, 2, bh, C_DIM);
        }
        if (hint) {
            gfx_set_text_datum(ui_cv, 0);
            gfx_set_text_color(ui_cv, C_DIM, 0, 0);
            gfx_draw_string(ui_cv, hint, 6, 125);
        }
        ui_present();
        int k = ui_key(-1);
        if (k == UI_UP && n) sel = (sel + n - 1) % n;
        else if (k == UI_DOWN && n) sel = (sel + 1) % n;
        else if (k == UI_ENTER && n) return sel;
        else if (k == UI_BACK) return -1;
    }
}

/* ---- WiFi ------------------------------------------------------------------------------ */
static void wifi_screen(void) {
    ui_status("Scanning...", "WiFi networks");
    static cpos_scan_t aps[20];
    int n = cpos_wifi_scan_blocking(aps, 20);
    if (n <= 0) { ui_message("WiFi", "No networks found."); return; }
    static ui_item_t items[20];
    for (int i = 0; i < n; i++) {
        snprintf(items[i].left, sizeof(items[i].left), "%.30s", aps[i].ssid);
        snprintf(items[i].right, sizeof(items[i].right), "%s%d", aps[i].open ? "open " : "", aps[i].rssi);
        items[i].color = 0;
    }
    int s = ui_menu("WiFi networks", items, n, 0, "enter: connect   `: back");
    if (s < 0) return;
    char pass[65] = "";
    if (!aps[s].open && !ui_input(aps[s].ssid, pass, sizeof(pass), 0)) return;
    ui_status("Connecting...", aps[s].ssid);
    if (cpos_wifi_connect(aps[s].ssid, pass, 15000) == 0) {
        cpos_wifi_save(aps[s].ssid, pass);
        ui_message("WiFi", "Connected. The network is saved.");
    } else {
        ui_message("WiFi", "Could not connect. Check the password.");
    }
}

/* ---- apps ----------------------------------------------------------------------------- */
typedef struct { char path[96]; char name[24]; char ver[12]; int ok; } app_entry_t;
static app_entry_t s_apps[40];
static int s_napps;

static int cmp_app(const void *a, const void *b) { return strcasecmp(((const app_entry_t *)a)->name, ((const app_entry_t *)b)->name); }

static void scan_apps(void) {
    s_napps = 0;
    mkdir("/sdcard/apps", 0777);
    DIR *d = opendir("/sdcard/apps");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_napps < 40) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".elf") != 0 || e->d_name[0] == '.') continue;
        app_entry_t *a = &s_apps[s_napps];
        snprintf(a->path, sizeof(a->path), "/sdcard/apps/%.80s", e->d_name);
        cpos_app_header_t h;
        const char *err = NULL;
        if (cpos_read_header(a->path, &h, &err) == 0) {
            strlcpy(a->name, h.name, sizeof(a->name));
            strlcpy(a->ver, h.version, sizeof(a->ver));
            a->ok = 1;
        } else {
            strlcpy(a->name, e->d_name, sizeof(a->name));
            a->ver[0] = 0;
            a->ok = 0;
        }
        s_napps++;
    }
    closedir(d);
    qsort(s_apps, s_napps, sizeof(s_apps[0]), cmp_app);
}

static void run_app(const app_entry_t *a) {
    ui_status("Starting...", a->name);
    const char *err = NULL;
    int rc = cpos_run_app(a->path, &err);
    if (rc < 0 && err) {
        char m[96];
        snprintf(m, sizeof(m), "%s: %s", a->name, err);
        ui_message("Could not start", m);
    }
}

static void system_screen(void) {
    char latest[16] = "";
    const esp_app_desc_t *d = esp_app_get_description();
    ui_item_t it[6];
    int n = 0;
    snprintf(it[n].left, 48, "OS version"); snprintf(it[n].right, 24, "%.20s", d->version); it[n].color = 0; n++;
    snprintf(it[n].left, 48, "Free heap"); snprintf(it[n].right, 24, "%u KB", (unsigned)(esp_get_free_heap_size() / 1024)); it[n].color = 0; n++;
    snprintf(it[n].left, 48, "Battery"); snprintf(it[n].right, 24, "%d mV (%d%%)", hal_batt_mv(), hal_batt_pct()); it[n].color = 0; n++;
    snprintf(it[n].left, 48, "App repo"); snprintf(it[n].right, 24, "%.22s", store_repo()); it[n].color = 0; n++;
    snprintf(it[n].left, 48, "Change app repo..."); it[n].right[0] = 0; it[n].color = C_ACC; n++;
    snprintf(it[n].left, 48, "Reboot"); it[n].right[0] = 0; it[n].color = C_ACC; n++;
    for (;;) {
        int s = ui_menu("System", it, n, 0, "`: back");
        if (s == 4) {
            char r[48];
            strlcpy(r, store_repo(), sizeof(r));
            if (ui_input("GitHub repo (owner/name)", r, sizeof(r), 0)) store_set_repo(r);
            snprintf(it[3].right, 24, "%.22s", store_repo());
        } else if (s == 5) {
            esp_restart();
        } else if (s < 0) break;
    }
    (void)latest;
}

void cpos_ui_run(void) {
    ui_cv = gfx_create(CPOS_SCREEN_W, CPOS_SCREEN_H, 16);
    char crash[96];
    cpos_boot_check(crash, sizeof(crash));
    if (crash[0]) ui_message("Previous app", crash);

    int sel = 0;
    for (;;) {
        scan_apps();
        static ui_item_t items[48];
        int n = 0;
        for (int i = 0; i < s_napps; i++, n++) {
            snprintf(items[n].left, 48, "%.28s", s_apps[i].name);
            snprintf(items[n].right, 24, "%s%s", s_apps[i].ok ? "v" : "", s_apps[i].ok ? s_apps[i].ver : "invalid");
            items[n].color = s_apps[i].ok ? 0 : C_ERR;
        }
        int apps_end = n;
        snprintf(items[n].left, 48, "+ App Store"); items[n].right[0] = 0; items[n].color = C_ACC; n++;
        snprintf(items[n].left, 48, "+ WiFi"); snprintf(items[n].right, 24, "%s", cpos_wifi_is_connected() ? "connected" : "off"); items[n].color = C_ACC; n++;
        snprintf(items[n].left, 48, "+ System"); items[n].right[0] = 0; items[n].color = C_ACC; n++;
        if (!hal_sd_ready()) {
            snprintf(items[n].left, 48, "No SD card"); items[n].right[0] = 0; items[n].color = C_ERR; n++;
        }
        if (sel >= n) sel = 0;
        sel = ui_menu("CardputerOS", items, n, sel, "enter: open   fn+`: exit app");
        if (sel < 0) { sel = 0; continue; }
        if (sel < apps_end) run_app(&s_apps[sel]);
        else if (sel == apps_end) store_screen();
        else if (sel == apps_end + 1) wifi_screen();
        else if (sel == apps_end + 2) system_screen();
    }
}
