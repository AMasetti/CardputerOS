#pragma once
#include <stdint.h>
#include "cpos/abi.h"

/* Software canvas engine. Colors are RGB565 (values above 0xFFFF are read as RGB888). */
#define CPOS_SCREEN_W 240
#define CPOS_SCREEN_H 135

cpos_canvas_t *gfx_create(int w, int h, int depth);
void gfx_destroy(cpos_canvas_t *c);
cpos_canvas_t *gfx_screen(void);
void gfx_push(cpos_canvas_t *src, cpos_canvas_t *dst, int x, int y, int transparent);
void gfx_flush_screen(void); /* send pending direct-draw changes to the panel */
void gfx_init(void);         /* starts the screen flush task */

void gfx_fill(cpos_canvas_t *c, uint32_t color);
void gfx_fill_rect(cpos_canvas_t *c, int x, int y, int w, int h, uint32_t color);
void gfx_set_font(cpos_canvas_t *c, int font);
void gfx_set_text_color(cpos_canvas_t *c, uint32_t fg, uint32_t bg, int has_bg);
void gfx_set_text_datum(cpos_canvas_t *c, int datum);
void gfx_set_text_size(cpos_canvas_t *c, float sx, float sy);
int gfx_draw_string(cpos_canvas_t *c, const char *s, int x, int y);
int gfx_text_width(cpos_canvas_t *c, const char *s);
int gfx_font_height(cpos_canvas_t *c);
void gfx_round_rect(cpos_canvas_t *c, int x, int y, int w, int h, int r, uint32_t color, int fill);
void gfx_hline(cpos_canvas_t *c, int x, int y, int w, uint32_t color);
void gfx_rect(cpos_canvas_t *c, int x, int y, int w, int h, uint32_t color, int fill);
void gfx_set_clip(cpos_canvas_t *c, int x, int y, int w, int h);
void gfx_clear_clip(cpos_canvas_t *c);
uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b);
void gfx_fill_api(cpos_gfx_api_t *api);

/* display driver (hal) */
void hal_display_init(void);
void hal_display_blit(int x, int y, int w, int h, const uint16_t *pix, int stride);
void hal_display_brightness(uint8_t v);
