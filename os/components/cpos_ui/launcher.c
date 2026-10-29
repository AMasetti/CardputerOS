/* Home screen: a carousel of rounded tiles, status pill on top, modifier keys on the left. */
#include "ui.h"
#include "app_ctx.h"
#include "hal.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>

#define C_BG gfx_rgb(12, 14, 22)
#define C_TXT 0xFFFF
#define C_DIM gfx_rgb(130, 135, 150)
#define C_ACC gfx_rgb(255, 120, 0)
#define C_PILL gfx_rgb(226, 222, 92)
#define C_INK gfx_rgb(30, 32, 40)
#define C_COL gfx_rgb(38, 42, 58)
#define C_TILE gfx_rgb(236, 238, 242)
#define C_TILE_DIM gfx_rgb(58, 62, 80)

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

static void draw_tile(const launcher_item_t *it, int x, int size, int focused) {
    int r = size / 5;
    uint16_t bg = focused ? C_TILE : C_TILE_DIM;
    uint16_t fg = focused ? it->color : gfx_rgb(200, 204, 215);
    int y = CY;
    if (focused) gfx_round_rect(ui_cv, x - size / 2 - 3, y - size / 2 - 3, size + 6, size + 6, r + 3, C_ACC, 1);
    gfx_round_rect(ui_cv, x - size / 2, y - size / 2, size, size, r, bg, 1);
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
    /* wifi */
    uint16_t wc = cpos_wifi_is_connected() ? C_INK : gfx_rgb(160, 156, 70);
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
    gfx_fill_rect(ui_cv, 204, 8, 22 * pct / 100, 7, pct < 20 ? gfx_rgb(220, 40, 40) : gfx_rgb(40, 150, 60));
}

static void draw_mods(const cpos_kbd_t *k) {
    static const char *lab[] = {"Aa", "fn", "ctrl", "opt", "alt"};
    int on[] = {k->shift, k->fn, k->ctrl, k->opt, k->alt};
    gfx_round_rect(ui_cv, 3, 3, 27, 129, 9, C_COL, 1);
    gfx_set_font(ui_cv, CPOS_FONT_JP12);
    gfx_set_text_datum(ui_cv, 5);
    for (int i = 0; i < 5; i++) {
        int y = 17 + i * 25;
        if (on[i]) gfx_round_rect(ui_cv, 5, y - 9, 23, 18, 5, C_ACC, 1);
        gfx_set_text_color(ui_cv, on[i] ? C_INK : C_DIM, 0, 0);
        gfx_draw_string(ui_cv, lab[i], 16, y);
    }
}

/* anim: -1..1, where the tiles sit relative to their final place (for the slide) */
static void draw(const launcher_item_t *items, int n, int sel, float anim, const cpos_kbd_t *k) {
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
            draw_tile(&items[i], CX + (int)(pos * STEP), size, d == 0 && ap < 0.5f);
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

int ui_launcher(const launcher_item_t *items, int n, int sel) {
    if (!G.arc) gfx_fill_api(&G);
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
