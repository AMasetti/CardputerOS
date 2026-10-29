/* Canvas engine: RGB565 software rendering shared by the OS UI and every app. */
#include "gfx.h"
#include "font.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct cpos_canvas {
    int w, h;
    uint16_t *buf;
    int is_screen;
    int cx0, cy0, cx1, cy1; /* clip, x1/y1 exclusive */
    int font;
    float sx, sy;
    uint16_t fg, bg;
    int has_bg;
    int datum;
    int dx0, dy0, dx1, dy1; /* dirty rect, screen only (empty when dx1 <= dx0) */
};

static cpos_canvas_t *s_screen;
static SemaphoreHandle_t s_screen_lock;

static const cpos_font_t *const s_fonts[CPOS_FONT_COUNT] = {
    [CPOS_FONT0] = NULL,           [CPOS_FONT2] = &cpos_font2,       [CPOS_FONT4] = &cpos_font4,
    [CPOS_FONT7] = &cpos_dejavu40, [CPOS_FONT_DEJAVU18] = &cpos_dejavu18, [CPOS_FONT_DEJAVU24] = &cpos_dejavu24,
    [CPOS_FONT_DEJAVU40] = &cpos_dejavu40, [CPOS_FONT_DEJAVU56] = &cpos_dejavu56,
    [CPOS_FONT_DEJAVU72] = &cpos_dejavu72, [CPOS_FONT_JP12] = &cpos_jp12, [CPOS_FONT_JP16] = &cpos_jp16,
};

uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

static inline uint16_t col16(uint32_t c) {
    if (c > 0xFFFF) return gfx_rgb(c >> 16, c >> 8, c);
    return (uint16_t)c;
}

/* ---- lifecycle ------------------------------------------------------------ */
/* The launcher's full-screen buffer is kept here while an app runs: the first big canvas the
 * app asks for (its screen or a full-screen sprite) gets it, so it never needs to find 64 KB in
 * one piece in a heap the app has already split. Released if the app runs out of memory. */
static uint16_t *s_spare;
static size_t s_spare_px;

void gfx_keep_spare(cpos_canvas_t *c) {
    if (!c) return;
    gfx_drop_spare();
    s_spare = c->buf;
    s_spare_px = (size_t)c->w * c->h;
    free(c);
}

int gfx_drop_spare(void) {
    if (!s_spare) return 0;
    free(s_spare);
    s_spare = NULL;
    return 1;
}

cpos_canvas_t *gfx_create(int w, int h, int depth) {
    (void)depth;
    if (w <= 0 || h <= 0) return NULL;
    cpos_canvas_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!c) return NULL;
    size_t px = (size_t)w * h;
    if (s_spare && px <= s_spare_px && px * 2 >= s_spare_px) {
        c->buf = s_spare;
        s_spare = NULL;
        memset(c->buf, 0, px * 2);
    } else {
        c->buf = heap_caps_calloc(px, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!c->buf && gfx_drop_spare()) c->buf = heap_caps_calloc(px, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!c->buf) {
        free(c);
        return NULL;
    }
    c->w = w;
    c->h = h;
    c->cx1 = w;
    c->cy1 = h;
    c->sx = c->sy = 1.0f;
    c->fg = 0xFFFF;
    return c;
}

void gfx_destroy(cpos_canvas_t *c) {
    if (!c || (c == s_screen && c->is_screen)) return;
    free(c->buf);
    free(c);
}

static void mark_dirty(cpos_canvas_t *c, int x0, int y0, int x1, int y1) {
    if (!c->is_screen) return;
    if (c->dx1 <= c->dx0) {
        c->dx0 = x0; c->dy0 = y0; c->dx1 = x1; c->dy1 = y1;
        return;
    }
    if (x0 < c->dx0) c->dx0 = x0;
    if (y0 < c->dy0) c->dy0 = y0;
    if (x1 > c->dx1) c->dx1 = x1;
    if (y1 > c->dy1) c->dy1 = y1;
}

cpos_canvas_t *gfx_screen(void) {
    if (!s_screen) {
        s_screen = gfx_create(CPOS_SCREEN_W, CPOS_SCREEN_H, 16);
        if (s_screen) s_screen->is_screen = 1;
    }
    return s_screen;
}

void gfx_flush_screen(void) {
    if (!s_screen || s_screen->dx1 <= s_screen->dx0) return;
    int x0 = s_screen->dx0, y0 = s_screen->dy0, x1 = s_screen->dx1, y1 = s_screen->dy1;
    s_screen->dx0 = s_screen->dy0 = s_screen->dx1 = s_screen->dy1 = 0;
    hal_display_blit(x0, y0, x1 - x0, y1 - y0, s_screen->buf + y0 * s_screen->w + x0, s_screen->w);
}

static void flush_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(16));
        xSemaphoreTake(s_screen_lock, portMAX_DELAY);
        gfx_flush_screen();
        xSemaphoreGive(s_screen_lock);
    }
}

/* the direct-draw framebuffer only exists while an app draws straight to the display */
void gfx_screen_release(void) {
    if (!s_screen) return;
    xSemaphoreTake(s_screen_lock, portMAX_DELAY);
    cpos_canvas_t *c = s_screen;
    s_screen = NULL;
    xSemaphoreGive(s_screen_lock);
    c->is_screen = 0;
    gfx_destroy(c);
}

void gfx_init(void) {
    s_screen_lock = xSemaphoreCreateMutex();
    xTaskCreate(flush_task, "gfx_flush", 3072, NULL, 3, NULL);
}

/* ---- pixels ----------------------------------------------------------------- */
void gfx_set_clip(cpos_canvas_t *c, int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    c->cx0 = x < 0 ? 0 : x;
    c->cy0 = y < 0 ? 0 : y;
    c->cx1 = x1 > c->w ? c->w : x1;
    c->cy1 = y1 > c->h ? c->h : y1;
}

void gfx_clear_clip(cpos_canvas_t *c) {
    c->cx0 = c->cy0 = 0;
    c->cx1 = c->w;
    c->cy1 = c->h;
}

void gfx_fill_rect(cpos_canvas_t *c, int x, int y, int w, int h, uint32_t color) {
    int x1 = x + w, y1 = y + h;
    if (x < c->cx0) x = c->cx0;
    if (y < c->cy0) y = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    if (x1 <= x || y1 <= y) return;
    uint16_t v = col16(color);
    uint32_t v2 = ((uint32_t)v << 16) | v;
    for (int yy = y; yy < y1; yy++) {
        uint16_t *p = c->buf + yy * c->w + x;
        int n = x1 - x;
        if (((uintptr_t)p & 3) && n) {
            *p++ = v;
            n--;
        }
        uint32_t *q = (uint32_t *)p;
        for (int k = n >> 1; k > 0; k--) *q++ = v2;
        if (n & 1) *(uint16_t *)q = v;
    }
    mark_dirty(c, x, y, x1, y1);
}

void gfx_fill(cpos_canvas_t *c, uint32_t color) {
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    (void)sx0; (void)sy0; (void)sx1; (void)sy1;
    gfx_fill_rect(c, 0, 0, c->w, c->h, color);
}

static inline void put(cpos_canvas_t *c, int x, int y, uint16_t v) {
    if (x < c->cx0 || x >= c->cx1 || y < c->cy0 || y >= c->cy1) return;
    c->buf[y * c->w + x] = v;
}

static void pixel(cpos_canvas_t *c, int x, int y, uint32_t color) {
    put(c, x, y, col16(color));
    mark_dirty(c, x, y, x + 1, y + 1);
}

void gfx_hline(cpos_canvas_t *c, int x, int y, int w, uint32_t color) { gfx_fill_rect(c, x, y, w, 1, color); }
static void vline(cpos_canvas_t *c, int x, int y, int h, uint32_t color) { gfx_fill_rect(c, x, y, 1, h, color); }

static void line(cpos_canvas_t *c, int x0, int y0, int x1, int y1, uint32_t color) {
    if (y0 == y1) {
        if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
        gfx_hline(c, x0, y0, x1 - x0 + 1, color);
        return;
    }
    if (x0 == x1) {
        if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
        vline(c, x0, y0, y1 - y0 + 1, color);
        return;
    }
    uint16_t v = col16(color);
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int minx = x0 < x1 ? x0 : x1, maxx = x0 < x1 ? x1 : x0;
    int miny = y0 < y1 ? y0 : y1, maxy = y0 < y1 ? y1 : y0;
    for (;;) {
        put(c, x0, y0, v);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    mark_dirty(c, minx, miny, maxx + 1, maxy + 1);
}

void gfx_rect(cpos_canvas_t *c, int x, int y, int w, int h, uint32_t color, int fill) {
    if (fill) {
        gfx_fill_rect(c, x, y, w, h, color);
        return;
    }
    if (w <= 0 || h <= 0) return;
    gfx_hline(c, x, y, w, color);
    gfx_hline(c, x, y + h - 1, w, color);
    vline(c, x, y + 1, h - 2, color);
    vline(c, x + w - 1, y + 1, h - 2, color);
}

/* circle quadrants: used for circles and rounded corners */
static void circle_pts(cpos_canvas_t *c, int cx, int cy, int r, uint32_t color, int fill, int dxw, int dyh) {
    /* dxw/dyh widen the shape so round rects reuse the code (0 for a circle) */
    int x = 0, y = r, d = 1 - r;
    uint16_t v = col16(color);
    while (x <= y) {
        if (fill) {
            gfx_hline(c, cx - x, cy - y, 2 * x + 1 + dxw, color);
            gfx_hline(c, cx - x, cy + y + dyh, 2 * x + 1 + dxw, color);
            gfx_hline(c, cx - y, cy - x, 2 * y + 1 + dxw, color);
            gfx_hline(c, cx - y, cy + x + dyh, 2 * y + 1 + dxw, color);
        } else {
            put(c, cx + x + dxw, cy + y + dyh, v); put(c, cx - x, cy + y + dyh, v);
            put(c, cx + x + dxw, cy - y, v);       put(c, cx - x, cy - y, v);
            put(c, cx + y + dxw, cy + x + dyh, v); put(c, cx - y, cy + x + dyh, v);
            put(c, cx + y + dxw, cy - x, v);       put(c, cx - y, cy - x, v);
        }
        if (d < 0) d += 2 * x + 3;
        else { d += 2 * (x - y) + 5; y--; }
        x++;
    }
    if (!fill) mark_dirty(c, cx - r, cy - r, cx + r + dxw + 1, cy + r + dyh + 1);
}

static void circle(cpos_canvas_t *c, int x, int y, int r, uint32_t color, int fill) {
    if (r < 0) return;
    circle_pts(c, x, y, r, color, fill, 0, 0);
}

void gfx_round_rect(cpos_canvas_t *c, int x, int y, int w, int h, int r, uint32_t color, int fill) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) {
        gfx_rect(c, x, y, w, h, color, fill);
        return;
    }
    if (fill) {
        /* corners: circles of radius r placed at the four corners, plus the middle bands */
        gfx_fill_rect(c, x + r, y, w - 2 * r, h, color);
        gfx_fill_rect(c, x, y + r, w, h - 2 * r, color); /* side strips between the corners */
        int x_l = x + r, x_r = x + w - 1 - r, y_t = y + r, y_b = y + h - 1 - r;
        int xs = 0, ys = r, d = 1 - r;
        while (xs <= ys) {
            gfx_hline(c, x_l - ys, y_t - xs, ys + 1, color); gfx_hline(c, x_r, y_t - xs, ys + 1, color);
            gfx_hline(c, x_l - ys, y_b + xs, ys + 1, color); gfx_hline(c, x_r, y_b + xs, ys + 1, color);
            gfx_hline(c, x_l - xs, y_t - ys, xs + 1, color); gfx_hline(c, x_r, y_t - ys, xs + 1, color);
            gfx_hline(c, x_l - xs, y_b + ys, xs + 1, color); gfx_hline(c, x_r, y_b + ys, xs + 1, color);
            if (d < 0) d += 2 * xs + 3;
            else { d += 2 * (xs - ys) + 5; ys--; }
            xs++;
        }
        return;
    }
    gfx_hline(c, x + r, y, w - 2 * r, color);
    gfx_hline(c, x + r, y + h - 1, w - 2 * r, color);
    vline(c, x, y + r, h - 2 * r, color);
    vline(c, x + w - 1, y + r, h - 2 * r, color);
    uint16_t v = col16(color);
    int x_l = x + r, x_r = x + w - 1 - r, y_t = y + r, y_b = y + h - 1 - r;
    int xs = 0, ys = r, d = 1 - r;
    while (xs <= ys) {
        put(c, x_r + xs, y_b + ys, v); put(c, x_r + ys, y_b + xs, v);
        put(c, x_l - xs, y_b + ys, v); put(c, x_l - ys, y_b + xs, v);
        put(c, x_r + xs, y_t - ys, v); put(c, x_r + ys, y_t - xs, v);
        put(c, x_l - xs, y_t - ys, v); put(c, x_l - ys, y_t - xs, v);
        if (d < 0) d += 2 * xs + 3;
        else { d += 2 * (xs - ys) + 5; ys--; }
        xs++;
    }
    mark_dirty(c, x, y, x + w, y + h);
}

static void ellipse(cpos_canvas_t *c, int cx, int cy, int rx, int ry, uint32_t color, int fill) {
    if (rx < 0 || ry < 0) return;
    if (rx == 0 || ry == 0) {
        line(c, cx - rx, cy - ry, cx + rx, cy + ry, color);
        return;
    }
    uint16_t v = col16(color);
    long rx2 = (long)rx * rx, ry2 = (long)ry * ry;
    for (int y = -ry; y <= ry; y++) {
        /* half-width at this scanline */
        int hw = (int)(rx * sqrtf(1.0f - (float)(y * y) / (float)ry2) + 0.5f);
        if (fill) gfx_hline(c, cx - hw, cy + y, 2 * hw + 1, color);
        else {
            /* outline: span between this row's edge and the next row's edge */
            int ny = y < 0 ? y + 1 : y - 1;
            int hw2 = (int)(rx * sqrtf(1.0f - (float)(ny * ny) / (float)ry2) + 0.5f);
            int a = hw < hw2 ? hw : hw2, b = hw < hw2 ? hw2 : hw;
            if (y == -ry || y == ry) { a = 0; b = hw; }
            for (int k = a; k <= b; k++) {
                put(c, cx - k, cy + y, v);
                put(c, cx + k, cy + y, v);
            }
        }
    }
    (void)rx2;
    mark_dirty(c, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
}

static void fill_tri(cpos_canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    int t;
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 > y2) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y0 == y2) {
        int a = x0, b = x0;
        if (x1 < a) a = x1;
        if (x1 > b) b = x1;
        if (x2 < a) a = x2;
        if (x2 > b) b = x2;
        gfx_hline(c, a, y0, b - a + 1, color);
        return;
    }
    for (int y = y0; y <= y2; y++) {
        int xa = x0 + (int)((long)(x2 - x0) * (y - y0) / (y2 - y0));
        int xb;
        if (y < y1) xb = (y1 == y0) ? x1 : x0 + (int)((long)(x1 - x0) * (y - y0) / (y1 - y0));
        else xb = (y2 == y1) ? x1 : x1 + (int)((long)(x2 - x1) * (y - y1) / (y2 - y1));
        if (xa > xb) { t = xa; xa = xb; xb = t; }
        gfx_hline(c, xa, y, xb - xa + 1, color);
    }
}

static void triangle(cpos_canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, int fill) {
    if (fill) fill_tri(c, x0, y0, x1, y1, x2, y2, color);
    else {
        line(c, x0, y0, x1, y1, color);
        line(c, x1, y1, x2, y2, color);
        line(c, x2, y2, x0, y0, color);
    }
}

/* Ring segment: angles in degrees, 0 = 3 o'clock, increasing clockwise on screen. */
static void arc(cpos_canvas_t *c, int cx, int cy, int r0, int r1, float a0, float a1, uint32_t color, int fill) {
    (void)fill;
    if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
    while (a1 < a0) a1 += 360.0f;
    if (a1 - a0 >= 360.0f) { a0 = 0; a1 = 360.0f; }
    uint16_t v = col16(color);
    for (int y = -r1; y <= r1; y++) {
        for (int x = -r1; x <= r1; x++) {
            int d2 = x * x + y * y;
            if (d2 > r1 * r1 || d2 < r0 * r0) continue;
            float a = atan2f((float)y, (float)x) * 57.2957795f;
            if (a < 0) a += 360.0f;
            if ((a >= a0 && a <= a1) || (a + 360.0f >= a0 && a + 360.0f <= a1)) put(c, cx + x, cy + y, v);
        }
    }
    mark_dirty(c, cx - r1, cy - r1, cx + r1 + 1, cy + r1 + 1);
}

/* ---- push ------------------------------------------------------------------- */
void gfx_push(cpos_canvas_t *src, cpos_canvas_t *dst, int x, int y, int transparent) {
    if (!src) return;
    if (!dst || dst == src) {
        /* to the panel: pushing the screen canvas itself just flushes it */
        if (src == s_screen) {
            xSemaphoreTake(s_screen_lock, portMAX_DELAY);
            mark_dirty(src, 0, 0, src->w, src->h);
            gfx_flush_screen();
            xSemaphoreGive(s_screen_lock);
            return;
        }
        int sx0 = 0, sy0 = 0, w = src->w, h = src->h;
        if (x < 0) { sx0 = -x; w += x; x = 0; }
        if (y < 0) { sy0 = -y; h += y; y = 0; }
        if (x + w > CPOS_SCREEN_W) w = CPOS_SCREEN_W - x;
        if (y + h > CPOS_SCREEN_H) h = CPOS_SCREEN_H - y;
        if (w <= 0 || h <= 0) return;
        if (s_screen) {
            /* keep the direct-draw framebuffer coherent with what is on the panel */
            xSemaphoreTake(s_screen_lock, portMAX_DELAY);
            for (int r = 0; r < h; r++) memcpy(s_screen->buf + (y + r) * s_screen->w + x, src->buf + (sy0 + r) * src->w + sx0, w * 2);
            s_screen->dx0 = s_screen->dy0 = s_screen->dx1 = s_screen->dy1 = 0;
            xSemaphoreGive(s_screen_lock);
        }
        hal_display_blit(x, y, w, h, src->buf + sy0 * src->w + sx0, src->w);
        return;
    }
    uint16_t tc = (uint16_t)transparent;
    for (int r = 0; r < src->h; r++) {
        int yy = y + r;
        if (yy < dst->cy0 || yy >= dst->cy1) continue;
        for (int q = 0; q < src->w; q++) {
            int xx = x + q;
            if (xx < dst->cx0 || xx >= dst->cx1) continue;
            uint16_t v = src->buf[r * src->w + q];
            if (transparent >= 0 && v == tc) continue;
            dst->buf[yy * dst->w + xx] = v;
        }
    }
    mark_dirty(dst, x < 0 ? 0 : x, y < 0 ? 0 : y, x + src->w > dst->w ? dst->w : x + src->w, y + src->h > dst->h ? dst->h : y + src->h);
}

/* ---- text ------------------------------------------------------------------- */
void gfx_set_font(cpos_canvas_t *c, int font) { c->font = (font >= 0 && font < CPOS_FONT_COUNT) ? font : 0; }
void gfx_set_text_size(cpos_canvas_t *c, float sx, float sy) {
    c->sx = sx > 0 ? sx : 1;
    c->sy = sy > 0 ? sy : c->sx;
}
void gfx_set_text_color(cpos_canvas_t *c, uint32_t fg, uint32_t bg, int has_bg) {
    c->fg = col16(fg);
    if (has_bg) c->bg = col16(bg);
    c->has_bg = has_bg;
}
void gfx_set_text_datum(cpos_canvas_t *c, int datum) { c->datum = datum; }
static void set_text_wrap(cpos_canvas_t *c, int wrap) { (void)c; (void)wrap; }

/* decode one UTF-8 code point */
static uint32_t next_cp(const char **ps) {
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = *s++;
    if (c < 0x80) { *ps = (const char *)s; return c; }
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (!extra) { *ps = (const char *)s; return '?'; }
    c &= (0x3F >> extra);
    while (extra-- && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3F);
    *ps = (const char *)s;
    return c;
}

static uint8_t fold_ascii(uint32_t cp) {
    /* the 6x8 font is ASCII only: strip accents from Latin-1 letters */
    static const char *tab = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    if (cp >= 0xC0 && cp <= 0xFF) return (uint8_t)tab[cp - 0xC0];
    if (cp == 0xB7 || cp == 0x2022) return '*';
    if (cp == 0xB0) return 0xF8; /* CP437 degree sign */
    return (cp < 0x80) ? (uint8_t)cp : '?';
}

static const cpos_glyph_t *find_glyph(const cpos_font_t *f, uint32_t cp) {
    for (int i = 0; i < f->count; i++)
        if (f->glyphs[i].cp == cp) return &f->glyphs[i];
    return NULL;
}

static inline int span(float s, int i) { /* pixel range of source pixel i when scaled by s */
    return (int)((i + 1) * s + 0.5f) - (int)(i * s + 0.5f);
}

int gfx_font_height(cpos_canvas_t *c) {
    const cpos_font_t *f = s_fonts[c->font];
    int h = f ? f->height : 8;
    return (int)(h * c->sy + 0.5f);
}

static int glyph_adv(cpos_canvas_t *c, const cpos_font_t *f, uint32_t cp) {
    if (!f) return (int)(6 * c->sx + 0.5f);
    const cpos_glyph_t *g = find_glyph(f, cp);
    if (!g) g = find_glyph(f, '?');
    return g ? (int)(g->adv * c->sx + 0.5f) : 0;
}

int gfx_text_width(cpos_canvas_t *c, const char *s) {
    const cpos_font_t *f = s_fonts[c->font];
    int w = 0;
    while (*s) w += glyph_adv(c, f, next_cp(&s));
    return w;
}

static void draw_bits(cpos_canvas_t *c, const uint8_t *rows, int rb, int w, int h, int x, int y, uint16_t v) {
    for (int r = 0; r < h; r++) {
        int y0 = y + (int)(r * c->sy + 0.5f);
        int rh = span(c->sy, r);
        for (int q = 0; q < w; q++) {
            if (!(rows[r * rb + (q >> 3)] & (0x80 >> (q & 7)))) continue;
            int x0 = x + (int)(q * c->sx + 0.5f);
            int rw = span(c->sx, q);
            int run = 1;
            while (q + run < w && (rows[r * rb + ((q + run) >> 3)] & (0x80 >> ((q + run) & 7)))) run++;
            if (run > 1) rw = (int)((q + run) * c->sx + 0.5f) - (int)(q * c->sx + 0.5f);
            if (rw == 1 && rh == 1) put(c, x0, y0, v);
            else {
                int sx0 = c->cx0, sx1 = c->cx1;
                (void)sx0; (void)sx1;
                for (int yy = 0; yy < rh; yy++)
                    for (int xx = 0; xx < rw; xx++) put(c, x0 + xx, y0 + yy, v);
            }
            q += run - 1;
        }
    }
}

int gfx_draw_string(cpos_canvas_t *c, const char *s, int x, int y) {
    const cpos_font_t *f = s_fonts[c->font];
    int total = gfx_text_width(c, s);
    int fh = gfx_font_height(c);
    int hd = c->datum & 3, vd = c->datum & 0x1C;
    int ascent = f ? f->ascent : 7;
    int asc_px = (int)(ascent * c->sy + 0.5f);
    switch (hd) { case 1: x -= total / 2; break; case 2: x -= total; break; default: break; }
    int top;
    if (vd == 16) top = y - asc_px;
    else if (vd == 8) top = y - fh;
    else if (vd == 4) top = y - fh / 2;
    else top = y;
    int base = top + asc_px;
    int x_start = x;

    while (*s) {
        uint32_t cp = next_cp(&s);
        int adv = glyph_adv(c, f, cp);
        if (c->has_bg) gfx_fill_rect(c, x, top, adv, fh, c->bg);
        if (!f) {
            uint8_t ch = fold_ascii(cp);
            uint8_t cols[5];
            memcpy(cols, cpos_glcd_font + ch * 5, 5);
            for (int q = 0; q < 5; q++) {
                for (int r = 0; r < 7; r++) {
                    if (!(cols[q] & (1 << r))) continue;
                    int px = x + (int)(q * c->sx + 0.5f), py = top + (int)(r * c->sy + 0.5f);
                    int rw = span(c->sx, q), rh = span(c->sy, r);
                    if (rw == 1 && rh == 1) put(c, px, py, c->fg);
                    else gfx_fill_rect(c, px, py, rw, rh, c->fg);
                }
            }
        } else {
            const cpos_glyph_t *g = find_glyph(f, cp);
            if (!g) g = find_glyph(f, '?');
            if (g && g->w) {
                int gx = x + (int)(g->xoff * c->sx), gy = base - (int)(g->ytop * c->sy + 0.5f);
                draw_bits(c, f->bitmap + g->off, (g->w + 7) / 8, g->w, g->h, gx, gy, c->fg);
            }
        }
        x += adv;
    }
    mark_dirty(c, x_start < 0 ? 0 : x_start, top < 0 ? 0 : top, x > c->w ? c->w : x, top + fh > c->h ? c->h : top + fh);
    return total;
}

static void blit(cpos_canvas_t *c, int x, int y, int w, int h, const uint16_t *pix, int swap, int transparent) {
    uint16_t tc = (uint16_t)transparent;
    for (int r = 0; r < h; r++) {
        int yy = y + r;
        if (yy < c->cy0 || yy >= c->cy1) continue;
        for (int q = 0; q < w; q++) {
            int xx = x + q;
            if (xx < c->cx0 || xx >= c->cx1) continue;
            uint16_t v = pix[r * w + q];
            if (swap) v = __builtin_bswap16(v);
            if (transparent >= 0 && v == tc) continue;
            c->buf[yy * c->w + xx] = v;
        }
    }
    mark_dirty(c, x < 0 ? 0 : x, y < 0 ? 0 : y, x + w > c->w ? c->w : x + w, y + h > c->h ? c->h : y + h);
}

/* ---- QR codes (encoder added in qr.c) ------------------------------------ */
void gfx_qrcode(cpos_canvas_t *c, const char *text, int x, int y, int size);

/* ---- table ----------------------------------------------------------------- */
static uint8_t *c_buffer(cpos_canvas_t *c) { return (uint8_t *)c->buf; }
static int c_width(cpos_canvas_t *c) { return c->w; }
static int c_height(cpos_canvas_t *c) { return c->h; }
static void c_arc(cpos_canvas_t *c, int x, int y, int r0, int r1, float a0, float a1, uint32_t col, int fill) { arc(c, x, y, r0, r1, a0, a1, col, fill); }

void gfx_fill_api(cpos_gfx_api_t *a) {
    a->create = gfx_create;
    a->destroy = gfx_destroy;
    a->screen = gfx_screen;
    a->buffer = c_buffer;
    a->width = c_width;
    a->height = c_height;
    a->push = gfx_push;
    a->fill = gfx_fill;
    a->pixel = pixel;
    a->line = line;
    a->hline = gfx_hline;
    a->vline = vline;
    a->rect = gfx_rect;
    a->round_rect = gfx_round_rect;
    a->circle = circle;
    a->ellipse = ellipse;
    a->triangle = triangle;
    a->arc = c_arc;
    a->set_font = gfx_set_font;
    a->set_text_size = gfx_set_text_size;
    a->set_text_color = gfx_set_text_color;
    a->set_text_datum = gfx_set_text_datum;
    a->set_text_wrap = set_text_wrap;
    a->draw_string = gfx_draw_string;
    a->text_width = gfx_text_width;
    a->font_height = gfx_font_height;
    a->set_clip = gfx_set_clip;
    a->clear_clip = gfx_clear_clip;
    a->qrcode = gfx_qrcode;
    a->set_brightness = hal_display_brightness;
    a->blit = blit;
}
