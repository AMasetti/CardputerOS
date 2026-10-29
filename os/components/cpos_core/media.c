/*
 * Decoders that run in the OS, so their code stays in flash instead of the app's executable
 * RAM: MP3 (Helix, third_party/libhelix-mp3) and JPEG (TJpgDec in the chip ROM).
 */
#include "app_ctx.h"
#include "esp_heap_caps.h"
#include "esp32s3/rom/tjpgd.h"
#include "third_party/libhelix-mp3/mp3dec.h"
#include <string.h>

/* memory hooks the Helix sources expect */
void *helix_malloc(int size) { return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
void helix_free(void *p) { heap_caps_free(p); }

/* ---- MP3 -------------------------------------------------------------------------------- */
static void *m_open(void) {
    if (!g_app) return NULL;
    int slot = -1;
    for (int i = 0; i < APP_MAX_DECODERS; i++) if (!g_app->mp3[i]) { slot = i; break; }
    if (slot < 0) return NULL;
    HMP3Decoder d = MP3InitDecoder();
    g_app->mp3[slot] = d;
    return d;
}

static void m_close(void *d) {
    if (!d) return;
    if (g_app) for (int i = 0; i < APP_MAX_DECODERS; i++) if (g_app->mp3[i] == d) g_app->mp3[i] = NULL;
    MP3FreeDecoder(d);
}

static int m_sync(const uint8_t *buf, int len) { return MP3FindSyncWord((unsigned char *)buf, len); }

static int m_decode(void *d, const uint8_t **in, int *left, int16_t *pcm, cpos_mp3_info_t *info) {
    unsigned char *p = (unsigned char *)*in;
    int rc = MP3Decode(d, &p, left, pcm, 0);
    *in = p;
    if (rc == 0 && info) {
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(d, &fi);
        info->rate = fi.samprate;
        info->channels = fi.nChans;
        info->samples = fi.outputSamps;
        info->bitrate = fi.bitrate;
    }
    return rc;
}

void cpos_app_cleanup_media(app_ctx_t *a) {
    for (int i = 0; i < APP_MAX_DECODERS; i++)
        if (a->mp3[i]) {
            MP3FreeDecoder(a->mp3[i]);
            a->mp3[i] = NULL;
        }
}

/* ---- JPEG ------------------------------------------------------------------------------- */
typedef struct { cpos_canvas_t *c; int ox, oy; cpos_read_fn read; void *ctx; } jsrc_t;

static UINT j_in(JDEC *d, BYTE *buf, UINT n) {
    jsrc_t *j = d->device;
    return j->read(j->ctx, buf, n);
}

static UINT j_out(JDEC *d, void *bmp, JRECT *r) {
    jsrc_t *j = d->device;
    const uint8_t *p = bmp;
    for (int y = r->top; y <= r->bottom; y++)
        for (int x = r->left; x <= r->right; x++, p += 3) gfx_put_pixel(j->c, j->ox + x, j->oy + y, gfx_rgb(p[0], p[1], p[2]));
    return 1;
}

static int j_draw(cpos_canvas_t *c, int x, int y, int mw, int mh, cpos_read_fn read, void *ctx) {
    if (!c || !read) return -1;
    void *pool = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pool) return -1;
    jsrc_t j = {c, x, y, read, ctx};
    JDEC d;
    int rc = -1;
    if (jd_prepare(&d, j_in, pool, 4096, &j) == JDR_OK) {
        int s = 0; /* scale 1/2^s until it fits the box */
        while (s < 3 && ((int)(d.width >> s) > mw || (int)(d.height >> s) > mh)) s++;
        j.ox = x + (mw - (int)(d.width >> s)) / 2;
        j.oy = y + (mh - (int)(d.height >> s)) / 2;
        rc = jd_decomp(&d, j_out, s) == JDR_OK ? 0 : -1;
        gfx_touch(c, x, y, mw, mh);
    }
    heap_caps_free(pool);
    return rc;
}

void cpos_media_fill(cpos_media_api_t *a) {
    a->mp3_open = m_open;
    a->mp3_close = m_close;
    a->mp3_sync = m_sync;
    a->mp3_decode = m_decode;
    a->jpeg_draw = j_draw;
}
