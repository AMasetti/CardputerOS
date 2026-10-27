#pragma once
#include <stdint.h>

/* Bitmap fonts: 1 bit per pixel, rows byte-aligned, MSB first. */
typedef struct {
    uint16_t cp;     /* unicode code point */
    uint16_t off;    /* offset into the bitmap blob */
    uint8_t w, h;
    int8_t xoff;     /* bitmap left bearing */
    int8_t ytop;     /* bitmap top above the baseline */
    uint8_t adv;     /* advance */
} cpos_glyph_t;

typedef struct {
    const uint8_t *bitmap;
    const cpos_glyph_t *glyphs;
    uint16_t count;
    uint8_t height;  /* line height */
    uint8_t ascent;
} cpos_font_t;

extern const uint8_t cpos_glcd_font[1280];
extern const cpos_font_t cpos_font2, cpos_font4, cpos_dejavu18, cpos_dejavu24, cpos_dejavu40,
    cpos_dejavu56, cpos_dejavu72, cpos_jp12, cpos_jp16;
