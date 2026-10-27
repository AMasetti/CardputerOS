#include "ui.h"
#include "hal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define C_BG gfx_rgb(12, 14, 22)
#define C_ACC gfx_rgb(255, 120, 0)
#define C_TXT 0xFFFF
#define C_DIM gfx_rgb(130, 135, 150)

/* Key press events with auto-repeat, built from the keyboard snapshot. */
int ui_key(int timeout_ms) {
    static cpos_kbd_t prev;
    static char held;       /* key that is repeating */
    static int64_t next_rep;
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        cpos_kbd_t k;
        hal_kbd_get(&k);
        int64_t now = esp_timer_get_time();
        int ev = UI_NONE;
        if (k.enter && !prev.enter) ev = UI_ENTER;
        else if (k.del && !prev.del) ev = UI_DEL;
        else if (k.space && !prev.space) ev = ' ';
        else if (k.tab && !prev.tab) ev = '\t';
        else {
            for (int i = 0; i < k.count; i++) {
                int was = 0;
                for (int j = 0; j < prev.count; j++) if (prev.keys[j] == k.keys[i]) was = 1;
                if (!was) { ev = k.keys[i]; break; }
            }
        }
        if (ev != UI_NONE) {
            held = ev;
            next_rep = now + 380000;
            prev = k;
            if (k.fn && ev == '`') ev = UI_NONE; /* Fn+` belongs to the system */
            if (ev) return ev;
        }
        prev = k;
        /* repeat */
        bool still = false;
        if (held == UI_ENTER) still = k.enter;
        else if (held == UI_DEL) still = k.del;
        else if (held == ' ') still = k.space;
        else for (int i = 0; i < k.count; i++) if (k.keys[i] == held) still = true;
        if (!still) held = 0;
        else if (now >= next_rep && held != UI_ENTER) {
            next_rep = now + 70000;
            return held;
        }
        if (hal_btn_a()) { while (hal_btn_a()) vTaskDelay(pdMS_TO_TICKS(10)); return UI_ENTER; }
        if (timeout_ms >= 0 && now >= end) return UI_NONE;
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

int ui_input(const char *title, char *buf, int max, int secret) {
    int len = strlen(buf);
    for (;;) {
        gfx_fill(ui_cv, C_BG);
        ui_header(title);
        gfx_set_font(ui_cv, CPOS_FONT_DEJAVU18);
        gfx_set_text_color(ui_cv, C_TXT, 0, 0);
        gfx_set_text_datum(ui_cv, 4);
        /* show the tail of the text when it does not fit */
        char shown[96];
        int sl = 0;
        for (int i = 0; i < len && sl < (int)sizeof(shown) - 2; i++) shown[sl++] = secret ? '*' : buf[i];
        shown[sl] = 0;
        const char *p = shown;
        while (gfx_text_width(ui_cv, p) > 214 && *p) p++;
        gfx_rect(ui_cv, 8, 40, 224, 30, C_ACC, 0);
        gfx_draw_string(ui_cv, p, 14, 55);
        int w = gfx_text_width(ui_cv, p);
        gfx_fill_rect(ui_cv, 14 + w + 1, 46, 2, 18, C_ACC);
        gfx_set_font(ui_cv, CPOS_FONT_JP12);
        gfx_set_text_color(ui_cv, C_DIM, 0, 0);
        gfx_set_text_datum(ui_cv, 0);
        gfx_draw_string(ui_cv, "enter: accept   `: cancel   del: erase", 8, 90);
        char cnt[24];
        snprintf(cnt, sizeof(cnt), "%d / %d", len, max - 1);
        gfx_draw_string(ui_cv, cnt, 8, 106);
        ui_present();
        int k = ui_key(-1);
        if (k == UI_ENTER) return 1;
        if (k == UI_BACK) return 0;
        if (k == UI_DEL) { if (len) buf[--len] = 0; }
        else if (k >= ' ' && k < 127 && len < max - 1) { buf[len++] = k; buf[len] = 0; }
    }
}
