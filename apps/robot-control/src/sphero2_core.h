// Protocolo Sphero v2 (R2-D2, R2-Q5, BB-9E, BOLT): paquetes con escape SLIP y UUIDs, sin dependencias.
//
// Paquete:  8D  FLAGS [TID SID] DID CID SEQ DATA... CHK  D8
//   CHK = ~suma(FLAGS..DATA) & 0xFF; dentro del cuerpo 8D -> AB 05, D8 -> AB 50, AB -> AB 23.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace sphero2 {

// UUIDs en little endian (como los entrega el ABI): 00010002-574f-4f20-5370-6865726f2121 y 00020005-...
static const uint8_t UUID_API[16] = {0x21, 0x21, 0x6f, 0x72, 0x65, 0x68, 0x70, 0x53, 0x20, 0x4f, 0x4f, 0x57, 0x02, 0x00, 0x01, 0x00};
static const uint8_t UUID_ANTIDOS[16] = {0x21, 0x21, 0x6f, 0x72, 0x65, 0x68, 0x70, 0x53, 0x20, 0x4f, 0x4f, 0x57, 0x05, 0x00, 0x02, 0x00};
static const uint8_t ANTIDOS_KEY[] = {'u', 's', 'e', 't', 'h', 'e', 'f', 'o', 'r', 'c', 'e', '.', '.', '.', 'b', 'a', 'n', 'd'};

enum Kind : uint8_t { R2D2, R2Q5, BB9E, BOLT };
inline const char *kind_name(Kind k) { static const char *n[] = {"R2-D2", "R2-Q5", "BB-9E", "BOLT"}; return n[k]; }
// prefijo anunciado: D2- Q5- GB- SB-
inline int kind_of(const char *name)
{
    if (!strncmp(name, "D2-", 3)) return R2D2;
    if (!strncmp(name, "Q5-", 3)) return R2Q5;
    if (!strncmp(name, "GB-", 3)) return BB9E;
    if (!strncmp(name, "SB-", 3)) return BOLT;
    return -1;
}

enum : uint8_t { DID_POWER = 0x13, DID_DRIVE = 0x16, DID_ANIMATRONIC = 0x17, DID_IO = 0x1A };
enum : uint8_t { DRIVE_FORWARD = 0, DRIVE_BACKWARD = 1, DRIVE_TURBO = 2 };
enum : uint8_t { LEG_STOP = 0, LEG_THREE = 1, LEG_TWO = 2, LEG_WADDLE = 3 };
// procesador de destino: BOLT maneja en el secundario (0x12) y despierta en el primario (0x11); el resto sin destino
enum : uint8_t { TID_NONE = 0, TID_PRIMARY = 0x11, TID_SECONDARY = 0x12 };

struct Builder {
    uint8_t seq = 0;

    // Arma el paquete ya escapado en `out` (>= 2 * (n + 9) + 2 bytes). Devuelve el largo.
    size_t packet(uint8_t *out, uint8_t did, uint8_t cid, uint8_t tid, const uint8_t *data, size_t n)
    {
        uint8_t body[48];
        size_t b = 0;
        body[b++] = tid ? 0x3A : 0x0A;  // pide respuesta | actividad [| tiene destino | tiene origen]
        if (tid) { body[b++] = tid; body[b++] = 0x01; }
        body[b++] = did;
        body[b++] = cid;
        body[b++] = seq;
        seq = (uint8_t)((seq + 1) % 0xFF);
        for (size_t i = 0; i < n && b < sizeof(body) - 1; i++) body[b++] = data[i];
        uint8_t sum = 0;
        for (size_t i = 0; i < b; i++) sum = (uint8_t)(sum + body[i]);
        body[b++] = (uint8_t)~sum;
        size_t o = 0;
        out[o++] = 0x8D;
        for (size_t i = 0; i < b; i++) {
            uint8_t c = body[i];
            if (c == 0xAB) { out[o++] = 0xAB; out[o++] = 0x23; }
            else if (c == 0x8D) { out[o++] = 0xAB; out[o++] = 0x05; }
            else if (c == 0xD8) { out[o++] = 0xAB; out[o++] = 0x50; }
            else out[o++] = c;
        }
        out[o++] = 0xD8;
        return o;
    }

    size_t wake(uint8_t *out, uint8_t tid = TID_NONE) { return packet(out, DID_POWER, 0x0D, tid, nullptr, 0); }
    size_t sleep(uint8_t *out, uint8_t tid = TID_NONE) { return packet(out, DID_POWER, 0x01, tid, nullptr, 0); }

    // velocidad 0..255 y rumbo 0..359 (grados, el 0 es "adelante" tras reset_yaw); velocidad 0 frena
    size_t drive(uint8_t *out, int speed, int heading, uint8_t flags, uint8_t tid)
    {
        if (speed < 0) speed = 0;
        if (speed > 255) speed = 255;
        heading = ((heading % 360) + 360) % 360;
        uint8_t d[4] = {(uint8_t)speed, (uint8_t)(heading >> 8), (uint8_t)(heading & 255), flags};
        return packet(out, DID_DRIVE, 0x07, tid, d, 4);
    }
    size_t reset_yaw(uint8_t *out, uint8_t tid) { return packet(out, DID_DRIVE, 0x06, tid, nullptr, 0); }

    // luces: máscara de 32 bits (un bit por LED) seguida de un valor por LED, de menor a mayor índice
    size_t leds(uint8_t *out, uint32_t mask, const uint8_t *vals, size_t nvals, uint8_t tid = TID_NONE)
    {
        uint8_t d[4 + 12];
        d[0] = (uint8_t)(mask >> 24); d[1] = (uint8_t)(mask >> 16); d[2] = (uint8_t)(mask >> 8); d[3] = (uint8_t)mask;
        if (nvals > 12) nvals = 12;
        memcpy(d + 4, vals, nvals);
        return packet(out, DID_IO, 26, tid, d, 4 + nvals);
    }
    size_t sound(uint8_t *out, uint16_t id, uint8_t mode = 0)
    {
        uint8_t d[3] = {(uint8_t)(id >> 8), (uint8_t)id, mode};
        return packet(out, DID_IO, 7, TID_NONE, d, 3);
    }
    size_t volume(uint8_t *out, uint8_t v) { return packet(out, DID_IO, 8, TID_NONE, &v, 1); }
    size_t stop_audio(uint8_t *out) { return packet(out, DID_IO, 10, TID_NONE, nullptr, 0); }
    // cabeza del R2-D2 en grados (-162..182), float big endian
    size_t head(uint8_t *out, float deg)
    {
        uint32_t u;
        memcpy(&u, &deg, 4);
        uint8_t d[4] = {(uint8_t)(u >> 24), (uint8_t)(u >> 16), (uint8_t)(u >> 8), (uint8_t)u};
        return packet(out, DID_ANIMATRONIC, 15, TID_NONE, d, 4);
    }
    size_t legs(uint8_t *out, uint8_t action) { return packet(out, DID_ANIMATRONIC, 13, TID_NONE, &action, 1); }
};

// Cuántos LED y cuáles (máscara) tiene el color principal de cada modelo: RGB del cuerpo/frente
inline uint32_t color_mask(Kind k) { return k == BOLT ? 0x3F : (k == BB9E ? 0x07 : 0x77); }
inline size_t color_vals(Kind k, uint8_t *v, uint8_t r, uint8_t g, uint8_t b)
{
    size_t n = 0;
    v[n++] = r; v[n++] = g; v[n++] = b;
    if (k == BOLT) { v[n++] = r; v[n++] = g; v[n++] = b; }       // frente y atrás
    else if (k != BB9E) { v[n++] = r; v[n++] = g; v[n++] = b; }  // R2: frente (0-2) y atrás (4-6)
    return n;
}

// Quita el escape de un paquete completo (8D ... D8) y comprueba el checksum. Devuelve el largo del
// cuerpo sin checksum en `body` (FLAGS..DATA), o -1.
inline int unpack(const uint8_t *p, size_t n, uint8_t *body, size_t max)
{
    if (n < 6 || p[0] != 0x8D || p[n - 1] != 0xD8) return -1;
    size_t b = 0;
    for (size_t i = 1; i + 1 < n; i++) {
        uint8_t c = p[i];
        if (c == 0xAB) {
            if (++i + 1 >= n) return -1;
            c = p[i] == 0x23 ? 0xAB : (p[i] == 0x05 ? 0x8D : (p[i] == 0x50 ? 0xD8 : 0));
            if (!c) return -1;
        }
        if (b >= max) return -1;
        body[b++] = c;
    }
    if (b < 2) return -1;
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < b; i++) sum = (uint8_t)(sum + body[i]);
    return (uint8_t)~sum == body[b - 1] ? (int)(b - 1) : -1;
}

}  // namespace sphero2
