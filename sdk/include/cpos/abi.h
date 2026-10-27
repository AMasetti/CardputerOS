/*
 * CardputerOS app ABI.
 *
 * This header is shared by the OS and by the app SDK. An app never links
 * against the OS: it receives a pointer to a cpos_api_t on entry and reaches
 * every service (display, keyboard, SD, WiFi, ...) through it. The OS owns all
 * hardware; apps declare which services they use in their header and the OS
 * blocks the rest.
 *
 * Versioning: CPOS_ABI_MAJOR changes when a function is removed or its meaning
 * changes. CPOS_ABI_MINOR changes when functions are appended. New function
 * pointers are only ever appended to the end of a struct, and an app states
 * the minimum minor it needs in its header; the OS refuses an app whose major
 * differs or whose minimum minor is newer than the OS.
 */
#ifndef CPOS_ABI_H
#define CPOS_ABI_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPOS_ABI_MAJOR 1
#define CPOS_ABI_MINOR 0

/* ---- permissions (declared in the app header) ----------------------------- */
#define CPOS_PERM_SD        (1u << 0)
#define CPOS_PERM_WIFI      (1u << 1)
#define CPOS_PERM_BLE       (1u << 2)
#define CPOS_PERM_AUDIO_OUT (1u << 3)
#define CPOS_PERM_AUDIO_IN  (1u << 4)
#define CPOS_PERM_IMU       (1u << 5)
#define CPOS_PERM_IR        (1u << 6)
#define CPOS_PERM_GPIO      (1u << 7) /* Grove / EXT header pins */

/* ---- app header, placed in the ".cpos_app" section ---------------------- */
#define CPOS_APP_MAGIC 0x50415043u /* "CPAP" */

typedef struct cpos_api cpos_api_t;
typedef int (*cpos_app_entry_t)(const cpos_api_t *api);

typedef struct {
    uint32_t magic;
    uint16_t abi_major;
    uint16_t abi_minor; /* minimum OS minor this app needs */
    uint32_t perms;
    uint32_t stack_size;
    cpos_app_entry_t entry;
    char name[24];
    char version[12];
} cpos_app_header_t;

/* ---- keyboard --------------------------------------------------------------- */
#define CPOS_KBD_MAX_KEYS 8

typedef struct {
    uint32_t seq;       /* increments on every press/release */
    uint8_t count;      /* number of entries in keys[] */
    char keys[CPOS_KBD_MAX_KEYS]; /* character keys held right now (shift applied) */
    uint8_t tab, fn, shift, ctrl, opt, alt, del, enter, space;
    uint8_t modifiers;  /* bit0 ctrl, bit1 shift, bit2 alt, bit3 opt, bit4 fn */
} cpos_kbd_t;

/* ---- graphics ------------------------------------------------------------- */
typedef struct cpos_canvas cpos_canvas_t;

enum {
    CPOS_FONT0 = 0,   /* 6x8 bitmap, scalable */
    CPOS_FONT2,       /* ~16 px proportional */
    CPOS_FONT4,       /* ~26 px proportional */
    CPOS_FONT7,       /* 7-segment digits */
    CPOS_FONT_DEJAVU18,
    CPOS_FONT_DEJAVU24,
    CPOS_FONT_DEJAVU40,
    CPOS_FONT_DEJAVU56,
    CPOS_FONT_DEJAVU72,
    CPOS_FONT_JP12,   /* small UI font (12 px) */
    CPOS_FONT_JP16,   /* small UI font (16 px) */
    CPOS_FONT_COUNT
};

typedef struct {
    cpos_canvas_t *(*create)(int w, int h, int depth); /* pixels are always RGB565; depth is accepted for source compatibility */
    void (*destroy)(cpos_canvas_t *c);
    cpos_canvas_t *(*screen)(void); /* draws land on the display shortly after */
    uint8_t *(*buffer)(cpos_canvas_t *c);
    int (*width)(cpos_canvas_t *c);
    int (*height)(cpos_canvas_t *c);
    /* copy src onto dst (NULL = the display) at x,y; transparent < 0: opaque */
    void (*push)(cpos_canvas_t *src, cpos_canvas_t *dst, int x, int y, int transparent);
    void (*fill)(cpos_canvas_t *c, uint32_t color);
    void (*pixel)(cpos_canvas_t *c, int x, int y, uint32_t color);
    void (*line)(cpos_canvas_t *c, int x0, int y0, int x1, int y1, uint32_t color);
    void (*hline)(cpos_canvas_t *c, int x, int y, int w, uint32_t color);
    void (*vline)(cpos_canvas_t *c, int x, int y, int h, uint32_t color);
    void (*rect)(cpos_canvas_t *c, int x, int y, int w, int h, uint32_t color, int fill);
    void (*round_rect)(cpos_canvas_t *c, int x, int y, int w, int h, int r, uint32_t color, int fill);
    void (*circle)(cpos_canvas_t *c, int x, int y, int r, uint32_t color, int fill);
    void (*ellipse)(cpos_canvas_t *c, int x, int y, int rx, int ry, uint32_t color, int fill);
    void (*triangle)(cpos_canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, int fill);
    void (*arc)(cpos_canvas_t *c, int x, int y, int r0, int r1, float a0, float a1, uint32_t color, int fill);
    void (*set_font)(cpos_canvas_t *c, int font);
    void (*set_text_size)(cpos_canvas_t *c, float sx, float sy);
    void (*set_text_color)(cpos_canvas_t *c, uint32_t fg, uint32_t bg, int has_bg);
    void (*set_text_datum)(cpos_canvas_t *c, int datum);
    void (*set_text_wrap)(cpos_canvas_t *c, int wrap);
    int (*draw_string)(cpos_canvas_t *c, const char *s, int x, int y); /* returns width drawn */
    int (*text_width)(cpos_canvas_t *c, const char *s);
    int (*font_height)(cpos_canvas_t *c);
    void (*set_clip)(cpos_canvas_t *c, int x, int y, int w, int h);
    void (*clear_clip)(cpos_canvas_t *c);
    void (*qrcode)(cpos_canvas_t *c, const char *text, int x, int y, int size);
    void (*set_brightness)(uint8_t v);
} cpos_gfx_api_t;

/* ---- audio, sensors, power, io ---------------------------------------------- */
typedef struct {
    int (*begin)(void);
    void (*end)(void);
    void (*set_volume)(uint8_t v);
    /* tone: wave may be NULL (sine). Non-blocking, mixes on channel (0..7). */
    void (*tone)(float freq, uint32_t ms, int channel, int stop_current, const uint8_t *wave, size_t wave_len);
    void (*play_raw)(const int16_t *samples, size_t len, uint32_t rate, int stereo, int repeat, int channel, int stop_current);
    int (*is_playing)(int channel); /* channel < 0: any */
    void (*stop)(int channel);      /* channel < 0: all */
    int (*mic_begin)(uint32_t rate);
    int (*mic_record)(int16_t *buf, size_t len, uint32_t rate); /* blocking until queued */
    void (*mic_end)(void);
} cpos_audio_api_t;

typedef struct {
    int (*update)(void);
    int (*enabled)(void);
    void (*accel)(float *x, float *y, float *z); /* g */
    void (*gyro)(float *x, float *y, float *z);  /* deg/s */
} cpos_imu_api_t;

typedef struct {
    int (*battery_level)(void);   /* 0..100 */
    int (*battery_mv)(void);
    int (*button_a)(void);        /* side button (GPIO0), 1 = pressed */
} cpos_power_api_t;

typedef struct {
    void (*pin_mode)(int pin, int mode); /* 0 input, 1 output, 2 input pullup */
    void (*write)(int pin, int level);
    int (*read)(int pin);
    int (*pwm_setup)(int channel, uint32_t freq, int bits);
    void (*pwm_attach)(int pin, int channel);
    void (*pwm_write)(int channel, uint32_t duty);
} cpos_gpio_api_t;

/* preferences: key/value store private to the app */
enum { CPOS_NVS_U8 = 1, CPOS_NVS_U16, CPOS_NVS_U32, CPOS_NVS_I32, CPOS_NVS_STR, CPOS_NVS_BLOB };
typedef struct {
    int (*open)(const char *ns, int read_only);          /* handle >= 0 */
    void (*close)(int h);
    int (*get)(int h, const char *key, int type, void *out, size_t *len); /* 0 ok, <0 missing */
    int (*set)(int h, const char *key, int type, const void *val, size_t len);
    int (*remove)(int h, const char *key);
    int (*clear)(int h);
} cpos_prefs_api_t;

/* ---- network ------------------------------------------------------------------- */
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth_open; /* 1 when no password */
    uint8_t channel;
} cpos_ap_t;

typedef struct {
    int (*mode)(int sta);                      /* start/stop the radio */
    int (*scan_start)(int async);              /* returns count when sync, 0 when async */
    int (*scan_count)(void);                   /* -1 running, >=0 done */
    int (*scan_get)(int i, cpos_ap_t *out);
    void (*scan_clear)(void);
    int (*connect)(const char *ssid, const char *pass);
    void (*disconnect)(void);
    int (*status)(void);                       /* 3 connected, 6 disconnected, 0 idle, 1 no ssid */
    uint32_t (*ip)(void);                      /* network order */
    uint32_t (*broadcast_ip)(void);
    int (*rssi)(void);
    const char *(*ssid)(void);
    void (*set_sleep)(int on);
    /* BSD sockets (lwip) */
    int (*socket)(int domain, int type, int proto);
    int (*bind)(int fd, const void *addr, uint32_t len);
    int (*sendto)(int fd, const void *buf, size_t len, int flags, const void *addr, uint32_t alen);
    int (*recvfrom)(int fd, void *buf, size_t len, int flags, void *addr, uint32_t *alen);
    int (*setsockopt)(int fd, int level, int opt, const void *val, uint32_t len);
    int (*fcntl)(int fd, int cmd, int arg);
    int (*close)(int fd);
} cpos_net_api_t;

/* ---- libc ---------------------------------------------------------------------- */
#include "libc_table.h"

/* ---- the table ------------------------------------------------------------------- */
struct cpos_api {
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t size;

    /* system */
    uint32_t (*millis)(void);
    uint32_t (*micros)(void);
    int64_t (*timer_us)(void);
    void (*delay_ms)(uint32_t ms);
    void (*delay_us)(uint32_t us);
    uint32_t (*random32)(void);
    void (*log)(const char *s);
    void *(*malloc)(size_t n);
    void *(*calloc)(size_t n, size_t sz);
    void *(*realloc)(void *p, size_t n);
    void (*free)(void *p);
    size_t (*free_heap)(void);
    int (*exit_requested)(void);
    void (*exit_app)(int code); /* does not return */
    void (*kbd_state)(cpos_kbd_t *out);

    cpos_gfx_api_t gfx;
    cpos_audio_api_t audio;
    cpos_imu_api_t imu;
    cpos_power_api_t power;
    cpos_gpio_api_t gpio;
    cpos_prefs_api_t prefs;
    cpos_net_api_t net;
    cpos_libc_t libc;
};

#ifdef __cplusplus
}
#endif

#endif /* CPOS_ABI_H */
