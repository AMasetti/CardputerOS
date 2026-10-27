#include "gfx.h"
#include "third_party/qrcodegen/qrcodegen.h"
#include <stdlib.h>

#define QR_MAX_VERSION 12

/* Draw a QR code for text inside a size x size square at x,y (white background, quiet zone included). */
void gfx_qrcode(cpos_canvas_t *c, const char *text, int x, int y, int size) {
    uint8_t *tmp = malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION));
    uint8_t *qr = malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION));
    if (!tmp || !qr) { free(tmp); free(qr); return; }
    if (qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, QR_MAX_VERSION, qrcodegen_Mask_AUTO, true)) {
        int n = qrcodegen_getSize(qr);
        int quiet = 2;
        int scale = size / (n + 2 * quiet);
        if (scale < 1) scale = 1;
        int total = (n + 2 * quiet) * scale;
        int ox = x + (size - total) / 2, oy = y + (size - total) / 2;
        gfx_fill_rect(c, ox, oy, total, total, 0xFFFF);
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++)
                if (qrcodegen_getModule(qr, i, j))
                    gfx_fill_rect(c, ox + (i + quiet) * scale, oy + (j + quiet) * scale, scale, scale, 0x0000);
    }
    free(tmp);
    free(qr);
}
