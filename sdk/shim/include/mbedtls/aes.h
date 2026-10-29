// mbedtls AES (ECB only) on top of the OS crypto service.
#pragma once
#include <cpos/abi.h>
#include <string.h>

#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_AES_DECRYPT 0

typedef struct { uint8_t key[32]; int bits; int enc; } mbedtls_aes_context;

extern const cpos_api_t *cpos_api;

static inline void mbedtls_aes_init(mbedtls_aes_context *c) { memset(c, 0, sizeof(*c)); }
static inline void mbedtls_aes_free(mbedtls_aes_context *c) { memset(c, 0, sizeof(*c)); }
static inline int mbedtls_aes_setkey_enc(mbedtls_aes_context *c, const unsigned char *k, unsigned bits) {
    if (bits != 128 && bits != 192 && bits != 256) return -0x20;
    memcpy(c->key, k, bits / 8);
    c->bits = bits;
    return 0;
}
static inline int mbedtls_aes_setkey_dec(mbedtls_aes_context *c, const unsigned char *k, unsigned bits) { return mbedtls_aes_setkey_enc(c, k, bits); }
static inline int mbedtls_aes_crypt_ecb(mbedtls_aes_context *c, int mode, const unsigned char in[16], unsigned char out[16]) {
    return cpos_api->crypto.aes_ecb(c->key, c->bits, mode == MBEDTLS_AES_ENCRYPT, in, out);
}
