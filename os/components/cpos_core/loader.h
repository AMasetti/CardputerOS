#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cpos/abi.h"

typedef struct {
    cpos_app_header_t header;   /* relocated copy, valid after cpos_load */
    void *code;                 /* executable memory (instruction-bus alias) */
    size_t code_size;
    void *data;
    size_t data_size;
    cpos_app_entry_t entry;
} cpos_image_t;

/* Read only the manifest (name, version, abi, permissions) without loading anything. */
int cpos_read_header(const char *path, cpos_app_header_t *out, const char **err);
/* Load and relocate an app into RAM. err is set to a static string on failure. */
int cpos_load(const char *path, cpos_image_t *img, const char **err);
void cpos_unload(cpos_image_t *img);
