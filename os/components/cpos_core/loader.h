#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cpos/abi.h"

#define CPOS_MAX_CODE_PARTS 8

typedef struct {
    cpos_app_header_t header;   /* relocated copy, valid after cpos_load */
    void *code;                 /* executable memory (instruction-bus alias): part[0] */
    size_t code_size;
    /* the code may be split at the cut points the build lists in .cpos_splits when no single
     * free block is big enough: part i holds code offsets part_off[i]..part_off[i+1] */
    int nparts;
    void *part[CPOS_MAX_CODE_PARTS];
    uint32_t part_off[CPOS_MAX_CODE_PARTS + 1];
    void *data;
    size_t data_size;
    cpos_app_entry_t entry;
} cpos_image_t;

/* Read only the manifest (name, version, abi, permissions) without loading anything. */
int cpos_read_header(const char *path, cpos_app_header_t *out, const char **err);
/* Launcher icon from the .cpos_icon section: a short vector drawing (ops ending with a 0 byte, see
 * tools/gen_icons.py) that the launcher draws at any size. 0 when the app has one. */
#define CPOS_ICON_MAX 160
int cpos_read_icon(const char *path, uint8_t *out);
/* The intents the app declares (compact JSON from the .cpos_intents section, NUL terminated); length or -1. */
int cpos_read_intents(const char *path, char *out, size_t max);
/* Load and relocate an app into RAM. err is set to a static string on failure. */
int cpos_load(const char *path, cpos_image_t *img, const char **err);
void cpos_unload(cpos_image_t *img);
