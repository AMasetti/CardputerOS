#pragma once
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cpos/abi.h"
#include "gfx.h"
#include "loader.h"

#define APP_MAX_FILES 12
#define APP_MAX_DIRS 4
#define APP_MAX_CANVAS 12
#define APP_MAX_FDS 8

typedef struct alloc_hdr {
    struct alloc_hdr *prev, *next;
    size_t size, pad;
} alloc_hdr_t;

/* Everything that belongs to the running app, released when it exits. */
typedef struct app_ctx {
    cpos_image_t img;
    uint32_t perms;
    volatile int exit_req;
    volatile int done;
    int result;
    jmp_buf jb;
    TaskHandle_t task;
    TaskHandle_t waiter;
    alloc_hdr_t *allocs;
    FILE *files[APP_MAX_FILES];
    DIR *dirs[APP_MAX_DIRS];
    cpos_canvas_t *canvases[APP_MAX_CANVAS];
    int fds[APP_MAX_FDS];
    int prefs_open[8];
    int pwm_pin[8];
    int audio_on, mic_on;
} app_ctx_t;

extern app_ctx_t *g_app;
extern const cpos_api_t g_api;

/* runner.c */
int cpos_run_app(const char *path, const char **err); /* blocks until the app ends */
void cpos_boot_check(char *msg, size_t n);           /* crash report of the previous boot, if any */

/* services shared with the UI */
void cpos_net_fill(cpos_net_api_t *n);
void cpos_audio_fill(cpos_audio_api_t *a);
void cpos_imu_fill(cpos_imu_api_t *a);
void cpos_app_cleanup_net(app_ctx_t *a);
void cpos_app_cleanup_audio(app_ctx_t *a);
void cpos_app_cleanup_gpio(app_ctx_t *a);
void cpos_app_cleanup_prefs(app_ctx_t *a);
int cpos_has_perm(uint32_t p);
void cpos_check_exit(void);

const cpos_api_t *cpos_api_get(void);
void cpos_app_release(app_ctx_t *a);

/* wifi service (shared by the OS UI and apps) */
typedef struct { char ssid[33]; int8_t rssi; uint8_t open; uint8_t channel; } cpos_scan_t;
void cpos_wifi_ensure(void);                       /* start the radio (idempotent) */
int cpos_wifi_scan_blocking(cpos_scan_t *out, int max);
int cpos_wifi_connect(const char *ssid, const char *pass, int timeout_ms); /* 0 on success */
void cpos_wifi_save(const char *ssid, const char *pass);
int cpos_wifi_load(char *ssid, char *pass);        /* 0 if credentials are stored */
int cpos_wifi_autoconnect(int timeout_ms);
int cpos_wifi_is_connected(void);
void cpos_wifi_stop(void);
