// mbedtls message digest: SHA-256 only, computed in the app (no OS call needed).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_SHA256 = 9 } mbedtls_md_type_t;
typedef struct { mbedtls_md_type_t type; } mbedtls_md_info_t;
typedef struct {
    const mbedtls_md_info_t *info;
    uint32_t h[8];
    uint64_t bits;
    uint8_t buf[64];
    size_t n;
} mbedtls_md_context_t;

static inline const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t t) {
    static const mbedtls_md_info_t sha256 = {MBEDTLS_MD_SHA256};
    return t == MBEDTLS_MD_SHA256 ? &sha256 : NULL;
}
static inline void mbedtls_md_init(mbedtls_md_context_t *c) { memset(c, 0, sizeof(*c)); }
static inline void mbedtls_md_free(mbedtls_md_context_t *c) { memset(c, 0, sizeof(*c)); }
static inline int mbedtls_md_setup(mbedtls_md_context_t *c, const mbedtls_md_info_t *info, int hmac) {
    if (!info || hmac) return -0x5100;
    c->info = info;
    return 0;
}
static inline unsigned char mbedtls_md_get_size(const mbedtls_md_info_t *info) { return info ? 32 : 0; }

static inline int mbedtls_md_starts(mbedtls_md_context_t *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv));
    c->bits = 0;
    c->n = 0;
    return 0;
}

static inline void cpos_sha256_block(uint32_t h[8], const uint8_t *p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define CPOS_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = CPOS_ROR(w[i - 15], 7) ^ CPOS_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = CPOS_ROR(w[i - 2], 17) ^ CPOS_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (CPOS_ROR(e, 6) ^ CPOS_ROR(e, 11) ^ CPOS_ROR(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (CPOS_ROR(a, 2) ^ CPOS_ROR(a, 13) ^ CPOS_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
#undef CPOS_ROR
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static inline int mbedtls_md_update(mbedtls_md_context_t *c, const unsigned char *in, size_t len) {
    c->bits += (uint64_t)len * 8;
    while (len) {
        size_t take = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, in, take);
        c->n += take;
        in += take;
        len -= take;
        if (c->n == 64) { cpos_sha256_block(c->h, c->buf); c->n = 0; }
    }
    return 0;
}

static inline int mbedtls_md_finish(mbedtls_md_context_t *c, unsigned char *out) {
    uint64_t bits = c->bits;
    uint8_t pad = 0x80;
    mbedtls_md_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) mbedtls_md_update(c, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = bits >> (56 - 8 * i);
    mbedtls_md_update(c, len, 8);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = c->h[i] >> (24 - 8 * j);
    return 0;
}
