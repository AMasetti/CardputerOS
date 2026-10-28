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
            gfx_draw_string(ui_cv, hint, 6, 121);
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

static void release_canvas(void) {
    gfx_destroy(ui_cv);
    ui_cv = NULL;
}

static void run_app(const app_entry_t *a) {
    ui_status("Starting...", a->name);
    vTaskDelay(pdMS_TO_TICKS(60)); /* let the launch click finish */
    cpos_ui_audio_off();
    /* the launcher's own canvas is 64 KB: give it back while the app runs */
    cpos_before_app_start = release_canvas;
    const char *err = NULL;
    int rc = cpos_run_app(a->path, &err);
    cpos_before_app_start = NULL;
    if (!ui_cv) ui_cv = gfx_create(CPOS_SCREEN_W, CPOS_SCREEN_H, 16);
    if (rc < 0 && err) {
        char m[96];
        snprintf(m, sizeof(m), "%s: %s", a->name, err);
        ui_message("Could not start", m);
    } else if (rc != 0) {
        char m[96];
        snprintf(m, sizeof(m), "%s stopped with an error (%d)%s", a->name, rc, rc == -5 ? ": not enough memory" : "");
        ui_message("App stopped", m);
    }
}

/* Test pattern + live adjustment of the panel orientation and window offset. */
static void display_screen(void) {
    int flip, gx, gy;
    hal_display_get_orient(&flip, &gx, &gy);
    for (;;) {
        gfx_fill(ui_cv, C_BG);
        gfx_rect(ui_cv, 0, 0, 240, 135, C_ACC, 0);          /* the whole panel must show this frame */
        gfx_rect(ui_cv, 1, 1, 238, 133, C_TXT, 0);
        gfx_fill_rect(ui_cv, 2, 2, 12, 12, C_ERR);            /* top-left red, top-right green, ... */
        gfx_fill_rect(ui_cv, 226, 2, 12, 12, C_OK);
        gfx_fill_rect(ui_cv, 2, 121, 12, 12, gfx_rgb(80, 140, 255));
        gfx_fill_rect(ui_cv, 226, 121, 12, 12, 0xFFFF);
        gfx_set_font(ui_cv, CPOS_FONT_JP12);
        gfx_set_text_datum(ui_cv, 5);
        gfx_set_text_color(ui_cv, C_TXT, 0, 0);
        gfx_draw_string(ui_cv, "Display setup", 120, 28);
        char b[64];
        snprintf(b, sizeof(b), "flip %d   x %d   y %d", flip, gx, gy);
        gfx_draw_string(ui_cv, b, 120, 50);
        gfx_set_text_color(ui_cv, C_DIM, 0, 0);
        gfx_draw_string(ui_cv, "text readable, frame complete?", 120, 72);
        gfx_draw_string(ui_cv, "f: flip   ; . : y   , / : x", 120, 92);
        gfx_draw_string(ui_cv, "enter: save    `: cancel", 120, 108);
        ui_present();
        int k = ui_key(-1);
        if (k == 'f') flip = (flip + 1) & 3;
        else if (k == UI_UP) gy--;
        else if (k == UI_DOWN) gy++;
        else if (k == UI_LEFT) gx--;
        else if (k == UI_RIGHT) gx++;
        else if (k == UI_ENTER) { hal_display_set_orient(flip, gx, gy, 1); return; }
        else if (k == UI_BACK) { hal_display_get_orient(&flip, &gx, &gy); return; }
        hal_display_set_orient(flip, gx, gy, 0);
    }
}

static void system_screen(void) {
    char latest[16] = "";
    const esp_app_desc_t *d = esp_app_get_description();
    ui_item_t it[8];
    int n = 0;
    snprintf(it[n].left, 48, "OS version"); snprintf(it[n].right, 24, "%.20s", d->version); it[n].color = 0; n++;
    snprintf(it[n].left, 48, "Free heap"); snprintf(it[n].right, 24, "%u KB", (unsigned)(esp_get_free_heap_size() / 1024)); it[n].color = 0; n++;
    snprintf(it[n].left, 48, "Battery"); snprintf(it[n].right, 24, "%d mV (%d%%)", hal_batt_mv(), hal_batt_pct()); it[n].color = 0; n++;
    snprintf(it[n].left, 48, "App repo"); snprintf(it[n].right, 24, "%.22s", store_repo()); it[n].color = 0; n++;
    snprintf(it[n].left, 48, "Change app repo..."); it[n].right[0] = 0; it[n].color = C_ACC; n++;
    snprintf(it[n].left, 48, "Display setup..."); it[n].right[0] = 0; it[n].color = C_ACC; n++;
    snprintf(it[n].left, 48, "Reboot"); it[n].right[0] = 0; it[n].color = C_ACC; n++;
    for (;;) {
        int s = ui_menu("System", it, n, 0, NULL);
        if (s == 4) {
            char r[48];
            strlcpy(r, store_repo(), sizeof(r));
            if (ui_input("GitHub repo (owner/name)", r, sizeof(r), 0)) store_set_repo(r);
            snprintf(it[3].right, 24, "%.22s", store_repo());
        } else if (s == 5) {
            display_screen();
        } else if (s == 6) {
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
        static launcher_item_t items[48];
        int n = 0;
        for (int i = 0; i < s_napps; i++, n++) {
            items[n] = (launcher_item_t){.kind = LI_APP, .color = launcher_color_for(s_apps[i].name)};
            snprintf(items[n].name, sizeof(items[n].name), "%s", s_apps[i].name);
            snprintf(items[n].sub, sizeof(items[n].sub), "%s%s", s_apps[i].ok ? "v" : "", s_apps[i].ok ? s_apps[i].ver : "invalid file");
            items[n].sub_color = s_apps[i].ok ? 0 : C_ERR;
        }
        int apps_end = n;
        items[n++] = (launcher_item_t){LI_STORE, "App Store", "get and update apps", gfx_rgb(40, 110, 230), 0};
        items[n] = (launcher_item_t){LI_WIFI, "WiFi", "", gfx_rgb(20, 160, 200), 0};
        snprintf(items[n].sub, sizeof(items[n].sub), "%s", cpos_wifi_is_connected() ? "connected" : "not connected");
        items[n++].sub_color = cpos_wifi_is_connected() ? C_OK : 0;
        items[n++] = (launcher_item_t){LI_SYSTEM, "System", "settings and info", gfx_rgb(110, 90, 200), 0};
        if (!hal_sd_ready()) items[n++] = (launcher_item_t){LI_NOSD, "No SD card", "insert one and reboot", C_ERR, C_ERR};
        if (sel >= n) sel = 0;
        sel = ui_launcher(items, n, sel);
        if (sel < apps_end) run_app(&s_apps[sel]);
        else if (sel == apps_end) store_screen();
        else if (sel == apps_end + 1) wifi_screen();
        else if (sel == apps_end + 2) system_screen();
    }
}
