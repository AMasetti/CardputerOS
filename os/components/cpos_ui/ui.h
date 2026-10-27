#pragma once
#include <stdint.h>
#include "gfx.h"

#define UI_ENTER '\n'
#define UI_DEL 8
#define UI_NONE 0
/* menu navigation keys: the arrows printed on the keycaps */
#define UI_UP ';'
#define UI_DOWN '.'
#define UI_LEFT ','
#define UI_RIGHT '/'
#define UI_BACK '`'

typedef struct { char left[48]; char right[24]; uint16_t color; } ui_item_t;

extern cpos_canvas_t *ui_cv;

void cpos_ui_run(void);                 /* never returns */
int ui_key(int timeout_ms);             /* next key press (with repeat) or UI_NONE */
void ui_present(void);
void ui_header(const char *title);
void ui_status(const char *l1, const char *l2); /* centered message while working */
void ui_message(const char *title, const char *text);
int ui_confirm(const char *title, const char *text);
int ui_menu(const char *title, ui_item_t *items, int n, int sel, const char *hint);
int ui_input(const char *title, char *buf, int max, int secret); /* 1 accepted, 0 cancelled */
void ui_progress(const char *title, const char *l2, int pct);

/* store.c */
void store_screen(void);
const char *store_repo(void);
void store_set_repo(const char *r);
int parse_version(const char *s, int v[3]);
