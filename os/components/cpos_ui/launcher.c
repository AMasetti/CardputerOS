/* Home screen: a carousel of rounded tiles or a grid of app icons (Tab switches), status pill on top,
 * modifier keys on the left. */
#include "ui.h"
#include "app_ctx.h"
#include "hal.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>
#include "nvs.h"
#include "loader.h"

#define C_BG gfx_rgb(4, 10, 20)
#define C_TXT gfx_rgb(215, 245, 252)
#define C_DIM gfx_rgb(80, 130, 150)
#define C_ACC gfx_rgb(64, 228, 255)     /* cyan: the one accent color */
#define C_EDGE gfx_rgb(24, 100, 130)    /* dim cyan outlines */
#define C_PILL gfx_rgb(8, 24, 42)
#define C_INK gfx_rgb(64, 228, 255)
#define C_COL gfx_rgb(8, 24, 42)
#define C_TILE gfx_rgb(10, 30, 52)
#define C_TILE_DIM gfx_rgb(7, 20, 36)

#define CX 136     /* carousel center (right of the modifier column) */
#define CY 60
#define STEP 70    /* distance between tiles */
#define BIG 58
#define SMALL 42

static cpos_gfx_api_t G;

static const uint16_t s_palette[] = {0xFB2C, 0x2D7F, 0x07F0, 0xFD20, 0xA81F, 0x05FF, 0xF81F, 0x87E0};

uint16_t launcher_color_for(const char *name) {
    unsigned h = 5381;
    for (const char *p = name; *p; p++) h = h * 33 + (unsigned char)*p;
    return s_palette[h % (sizeof(s_palette) / sizeof(s_palette[0]))];
}

static void icon_wifi(int x, int y, int s, uint16_t c, uint16_t bg) {
    int r = s * 30 / 100;
    for (int i = 0; i < 3; i++) {
        int ro = r - i * (r / 3);
        G.arc(ui_cv, x, y + r / 2, ro - s / 22 - 1, ro, 225, 315, c, 1);
    }
    G.circle(ui_cv, x, y + r / 2, s / 18 + 1, c, 1);
    (void)bg;
}

static void icon_store(int x, int y, int s, uint16_t c, uint16_t bg) {
    int w = s * 44 / 100, h = s * 30 / 100;
    gfx_round_rect(ui_cv, x - w / 2, y - h / 2 + s / 10, w, h, 3, c, 1);          /* bag */
    G.arc(ui_cv, x, y - h / 2 + s / 10, w / 4 - 2, w / 4, 180, 360, c, 1);         /* handle */
    G.triangle(ui_cv, x - w / 5, y, x + w / 5, y, x, y + h / 3, bg, 1);            /* arrow */
    gfx_fill_rect(ui_cv, x - 2, y - h / 4, 4, h / 4 + 1, bg);
}

static void icon_system(int x, int y, int s, uint16_t c, uint16_t bg) {
    int r = s * 22 / 100;
    for (int i = 0; i < 8; i++) {            /* gear teeth */
        static const int8_t dx[] = {10, 7, 0, -7, -10, -7, 0, 7}, dy[] = {0, 7, 10, 7, 0, -7, -10, -7};
        G.circle(ui_cv, x + dx[i] * r * 13 / 100, y + dy[i] * r * 13 / 100, s / 14 + 1, c, 1);
    }
    G.circle(ui_cv, x, y, r, c, 1);
    G.circle(ui_cv, x, y, r / 2, bg, 1);
}

/* the app's own icon, scaled (nearest) to size x size; palette index 0 is transparent */
static void draw_icon(const uint8_t *ic, int cx, int cy, int size, int dim) {
    uint16_t pal[16];
    for (int i = 0; i < 16; i++) {
        uint16_t c = ic[i * 2] | (ic[i * 2 + 1] << 8);
        if (dim) c = (((c >> 11) * 5 / 8) << 11) | ((((c >> 5) & 63) * 5 / 8) << 5) | ((c & 31) * 5 / 8); /* not focused: darker */
        pal[i] = c;
    }
    const uint8_t *px = ic + 32;
    int x0 = cx - size / 2, y0 = cy - size / 2;
    for (int y = 0; y < size; y++) {
        int sy = y * CPOS_ICON_SIZE / size;
        for (int x = 0; x < size; x++) {
            int sx = x * CPOS_ICON_SIZE / size;
            uint8_t b = px[(sy * CPOS_ICON_SIZE + sx) / 2];
            int v = (sx & 1) ? (b & 15) : (b >> 4);
            if (v) gfx_put_pixel(ui_cv, x0 + x, y0 + y, pal[v]);
        }
    }
}

static void draw_tile(const launcher_item_t *it, int x, int y, int size, int focused) {
    int r = size / 5;
    uint16_t bg = focused ? C_TILE : C_TILE_DIM;
    uint16_t fg = focused ? C_ACC : C_EDGE;
    if (focused) gfx_round_rect(ui_cv, x - size / 2 - 3, y - size / 2 - 3, size + 6, size + 6, r + 3, C_ACC, 1);
    if (it->icon) { /* the icon is the whole tile; dim it when not focused */
        draw_icon(it->icon, x, y, size, !focused);
        return;
    }
    gfx_round_rect(ui_cv, x - size / 2, y - size / 2, size, size, r, bg, 1);
    gfx_round_rect(ui_cv, x - size / 2, y - size / 2, size, size, r, focused ? C_ACC : C_EDGE, 0);
    gfx_round_rect(ui_cv, x - size / 2 + 1, y - size / 2 + 1, size - 2, size - 2, r - 1, focused ? C_ACC : C_EDGE, 0);
    switch (it->kind) {
        case LI_WIFI: icon_wifi(x, y, size, fg, bg); break;
        case LI_STORE: icon_store(x, y, size, fg, bg); break;
        case LI_SYSTEM: icon_system(x, y, size, fg, bg); break;
        default: {
            char l[3] = {it->kind == LI_NOSD ? 'S' : it->name[0], it->kind == LI_NOSD ? 'D' : 0, 0};
            if (l[0] >= 'a' && l[0] <= 'z') l[0] -= 32;
            gfx_set_font(ui_cv, size >= BIG - 4 ? CPOS_FONT_DEJAVU40 : CPOS_FONT_DEJAVU24);
            gfx_set_text_datum(ui_cv, 4 + 1); /* middle center */
            gfx_set_text_color(ui_cv, fg, 0, 0);
            gfx_draw_string(ui_cv, l, x, y + 2);
        }
    }
}

static void draw_status(void) {
    gfx_round_rect(ui_cv, 34, 3, 203, 17, 8, C_PILL, 1);
    gfx_round_rect(ui_cv, 34, 3, 203, 17, 8, C_EDGE, 0);
    /* wifi */
    uint16_t wc = cpos_wifi_is_connected() ? C_INK : C_EDGE;
    for (int i = 0; i < 3; i++) G.arc(ui_cv, 50, 16, 9 - i * 3 - 1, 9 - i * 3, 225, 315, wc, 1);
    G.circle(ui_cv, 50, 15, 1, wc, 1);
    /* local time once it was fetched from the network; until then, time since power on */
    char t[12];
    if (cpos_time_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(t, sizeof(t), "%H:%M", &tm);
    } else {
        int64_t m = esp_timer_get_time() / 60000000;
        snprintf(t, sizeof(t), "%02d:%02d", (int)(m / 60 % 100), (int)(m % 60));
    }
    gfx_set_font(ui_cv, CPOS_FONT_JP12);
    gfx_set_text_datum(ui_cv, 5);
    gfx_set_text_color(ui_cv, C_INK, 0, 0);
    gfx_draw_string(ui_cv, t, 136, 11);
    /* battery */
    int pct = hal_batt_pct();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_round_rect(ui_cv, 202, 6, 26, 11, 3, C_INK, 0);
    gfx_fill_rect(ui_cv, 228, 9, 2, 5, C_INK);
    gfx_fill_rect(ui_cv, 204, 8, 22 * pct / 100, 7, pct < 20 ? gfx_rgb(255, 80, 80) : C_ACC);
}

static void draw_mods(const cpos_kbd_t *k) {
    static const char *lab[] = {"Aa", "fn", "ctrl", "opt", "alt"};
    int on[] = {k->shift, k->fn, k->ctrl, k->opt, k->alt};
    gfx_round_rect(ui_cv, 3, 3, 27, 129, 9, C_COL, 1);
    gfx_round_rect(ui_cv, 3, 3, 27, 129, 9, C_EDGE, 0);
    gfx_set_font(ui_cv, CPOS_FONT_JP12);
    gfx_set_text_datum(ui_cv, 5);
    for (int i = 0; i < 5; i++) {
        int y = 17 + i * 25;
        if (on[i]) gfx_round_rect(ui_cv, 5, y - 9, 23, 18, 5, C_ACC, 1);
        gfx_set_text_color(ui_cv, on[i] ? C_BG : C_DIM, 0, 0);
        gfx_draw_string(ui_cv, lab[i], 16, y);
    }
}

/* anim: -1..1, where the tiles sit relative to their final place (for the slide) */
static void draw_carousel(const launcher_item_t *items, int n, int sel, float anim, const cpos_kbd_t *k) {
    gfx_fill(ui_cv, C_BG);
    draw_status();
    draw_mods(k);
    gfx_set_clip(ui_cv, 33, 22, 207, 113);
    /* far tiles first so the focused one is drawn on top */
    for (int d = 3; d >= 0; d--) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            if (d == 0 && sgn > 0) continue;
            int i = sel + sgn * d;
            if (i < 0 || i >= n) continue;
            float pos = sgn * d + anim;
            float ap = pos < 0 ? -pos : pos;
            int size = ap >= 1 ? SMALL : (int)(BIG - (BIG - SMALL) * ap);
            draw_tile(&items[i], CX + (int)(pos * STEP), CY, size, d == 0 && ap < 0.5f);
        }
    }
    gfx_clear_clip(ui_cv);
    if (anim > -0.3f && anim < 0.3f) {
        const launcher_item_t *it = &items[sel];
        gfx_set_font(ui_cv, CPOS_FONT_JP16);
        gfx_set_text_datum(ui_cv, 5);
        gfx_set_text_color(ui_cv, C_TXT, 0, 0);
        gfx_draw_string(ui_cv, it->name, CX, 102);
        gfx_set_font(ui_cv, CPOS_FONT_JP12);
        const char *sub = it->sub;
        uint16_t sc = it->sub_color ? it->sub_color : C_DIM;
        if (it->kind == LI_WIFI) { /* live: the connection comes up in the background */
            int on = cpos_wifi_is_connected();
            sub = on ? "connected" : cpos_net_background_busy() ? "connecting..." : "not connected";
            sc = on ? gfx_rgb(80, 220, 120) : C_DIM;
        }
        gfx_set_text_color(ui_cv, sc, 0, 0);
        gfx_draw_string(ui_cv, sub, CX, 118);
    }
    /* position */
    if (n <= 14) {
        int x0 = CX - (n - 1) * 4;
        for (int i = 0; i < n; i++) G.circle(ui_cv, x0 + i * 8, 129, i == sel ? 2 : 1, i == sel ? C_ACC : C_DIM, 1);
    } else {
        char p[12];
        snprintf(p, sizeof(p), "%d/%d", sel + 1, n);
        gfx_set_font(ui_cv, CPOS_FONT_JP12);
        gfx_set_text_datum(ui_cv, 10); /* bottom right */
        gfx_set_text_color(ui_cv, C_DIM, 0, 0);
        gfx_draw_string(ui_cv, p, 236, 133);
    }
    ui_present();
}

/* ---- grid: 4 x 2 icons, scrolls by rows ------------------------------------------------ */
#define GCOLS 4
#define GROWS 2
#define GW 51      /* cell width: 4 * 51 = 204 fits right of the modifier column */
#define GH 55
#define GX0 34
#define GY0 22
#define GICON 36

static void draw_grid(const launcher_item_t *items, int n, int sel, int top, const cpos_kbd_t *k) {
    gfx_fill(ui_cv, C_BG);
    draw_status();
    draw_mods(k);
    gfx_set_clip(ui_cv, 33, 22, 207, 113);
    gfx_set_font(ui_cv, CPOS_FONT_JP12);
    for (int r = 0; r < GROWS; r++) {
        for (int c = 0; c < GCOLS; c++) {
            int i = (top + r) * GCOLS + c;
            if (i >= n) break;
            int cx = GX0 + c * GW + GW / 2, cy = GY0 + r * GH + 19;
            int on = i == sel;
            if (on) {
                gfx_round_rect(ui_cv, GX0 + c * GW + 1, GY0 + r * GH + 1, GW - 2, GH - 3, 8, C_COL, 1);
                gfx_round_rect(ui_cv, GX0 + c * GW + 1, GY0 + r * GH + 1, GW - 2, GH - 3, 8, C_ACC, 0);
            }
            draw_tile(&items[i], cx, cy, GICON, on);
            /* name, cut to the cell */
            char nm[24];
            strlcpy(nm, items[i].name, sizeof(nm));
            int len = strlen(nm);
            while (len > 1 && gfx_text_width(ui_cv, nm) > GW - 4) nm[--len] = 0;
            gfx_set_text_datum(ui_cv, 5);
            gfx_set_text_color(ui_cv, on ? C_TXT : C_DIM, 0, 0);
            gfx_draw_string(ui_cv, nm, cx, GY0 + r * GH + 47);
        }
    }
    gfx_clear_clip(ui_cv);
    int rows = (n + GCOLS - 1) / GCOLS;
    if (rows > GROWS) { /* scroll bar */
        int h = 110 / rows * GROWS;
        gfx_fill_rect(ui_cv, 237, 22 + (110 - h) * top / (rows - GROWS), 2, h, C_DIM);
    }
    ui_present();
}

static int s_grid = -1;

static void grid_load(void) {
    s_grid = 0;
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("os", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "grid", &v);
        nvs_close(h);
    }
    s_grid = v;
}

static void grid_save(void) {
    nvs_handle_t h;
    if (nvs_open("os", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "grid", s_grid);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void draw(const launcher_item_t *items, int n, int sel, float anim, const cpos_kbd_t *k) {
    if (!s_grid) { draw_carousel(items, n, sel, anim, k); return; }
    int row = sel / GCOLS, rows = (n + GCOLS - 1) / GCOLS;
    static int top;
    if (row < top) top = row;
    if (row >= top + GROWS) top = row - GROWS + 1;
    if (top > rows - GROWS) top = rows > GROWS ? rows - GROWS : 0;
    draw_grid(items, n, sel, top, k);
}

int ui_launcher(const launcher_item_t *items, int n, int sel) {
    if (!G.arc) gfx_fill_api(&G);
    if (s_grid < 0) grid_load();
    if (sel < 0 || sel >= n) sel = 0;
    cpos_kbd_t k, shown;
    hal_kbd_get(&k);
    shown = k;
    int64_t shown_min = -1;
    int shown_wifi = -1;
    draw(items, n, sel, 0, &k);
    for (;;) {
        int key = ui_key(80);
        int dir = 0;
        if (key == '\t') { /* carousel <-> grid, same selection */
            s_grid = !s_grid;
            grid_save();
            cpos_ui_beep(1800, 14);
            hal_kbd_get(&k);
            draw(items, n, sel, 0, &k);
            continue;
        }
        if (s_grid && n > 1 && (key == UI_UP || key == UI_DOWN || key == UI_LEFT || key == UI_RIGHT)) {
            int t = sel;
            if (key == UI_LEFT) t = (sel + n - 1) % n;
            else if (key == UI_RIGHT) t = (sel + 1) % n;
            else if (key == UI_UP) t = sel >= GCOLS ? sel - GCOLS : sel;
            else t = sel + GCOLS < n ? sel + GCOLS : (sel / GCOLS < (n - 1) / GCOLS ? n - 1 : sel);
            if (t != sel) { sel = t; cpos_ui_beep(2400, 14); }
            hal_kbd_get(&k);
            draw(items, n, sel, 0, &k);
            continue;
        }
        if (key == UI_RIGHT || key == UI_DOWN) dir = 1;
        else if (key == UI_LEFT || key == UI_UP) dir = -1;
        else if (key == UI_ENTER && n) { cpos_ui_beep(1300, 45); return sel; }
        if (dir && n > 1) {
            sel = (sel + dir + n) % n;
            cpos_ui_beep(2400, 14);
            hal_kbd_get(&k);
            for (int f = 3; f >= 0; f--) draw(items, n, sel, dir * f / 4.0f, &k);
            continue;
        }
        /* idle: redraw only when something on screen changed */
        hal_kbd_get(&k);
        int64_t min = cpos_time_valid() ? time(NULL) / 60 : esp_timer_get_time() / 60000000;
        int wifi = cpos_wifi_is_connected();
        if (k.shift != shown.shift || k.fn != shown.fn || k.ctrl != shown.ctrl || k.opt != shown.opt ||
            k.alt != shown.alt || min != shown_min || wifi != shown_wifi) {
            shown = k;
            shown_min = min;
            shown_wifi = wifi;
            draw(items, n, sel, 0, &k);
        }
    }
}
