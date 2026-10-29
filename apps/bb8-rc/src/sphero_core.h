// Protocolo Sphero v1 (BB-8 original, Ollie, Sphero 2.0, SPRK): paquetes y UUIDs, sin dependencias.
//
// Paquete de comando:  FF  SOP2  DID  CID  SEQ  DLEN  DATA...  CHK
//   SOP2 = 0xFC | 1 (pedir respuesta) | 2 (reiniciar el timeout del robot)
//   DLEN = len(DATA) + 1,  CHK = ~(DID + CID + SEQ + DLEN + sum(DATA)) & 0xFF
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace sphero {

// UUIDs: 22bb746f-2bXX-7554-2d6f-726568705327. En el ABI de BLE van en little endian.
static const uint8_t UUID_TAIL[10] = {0x75, 0x54, 0x2d, 0x6f, 0x72, 0x65, 0x68, 0x70, 0x53, 0x27};
enum Chr : uint8_t { CHR_ANTIDOS = 0xbd, CHR_TXPOWER = 0xb2, CHR_WAKE = 0xbf, CHR_CMD = 0xa1, CHR_NOTIFY = 0xa6 };

// ¿`uuid` (128 bits, little endian) es 22bb746f-2b<chr>-7554-...?
inline bool uuid_is(const uint8_t uuid[16], uint8_t chr)
{
    // big endian: 22 bb 74 6f 2b CHR 75 54 2d 6f 72 65 68 70 53 27 -> little endian al revés
    if (uuid[0] != 0x27 || uuid[1] != 0x53 || uuid[2] != 0x70 || uuid[3] != 0x68 || uuid[4] != 0x65 ||
        uuid[5] != 0x72 || uuid[6] != 0x6f || uuid[7] != 0x2d || uuid[8] != 0x54 || uuid[9] != 0x75)
        return false;
    return uuid[10] == chr && uuid[11] == 0x2b && uuid[12] == 0x6f && uuid[13] == 0x74 && uuid[14] == 0xbb && uuid[15] == 0x22;
}

// Secuencia de despertar (cada escritura con respuesta, en este orden)
static const uint8_t WAKE_ANTIDOS[5] = {'0', '1', '1', 'i', '3'};
static const uint8_t WAKE_TXPOWER[1] = {0x07};
static const uint8_t WAKE_CPU[1] = {0x01};

enum : uint8_t { DID_CORE = 0x00, DID_SPHERO = 0x02 };
enum : uint8_t { CID_SLEEP = 0x22, CID_HEADING = 0x01, CID_RGB = 0x20, CID_BACKLED = 0x21, CID_ROLL = 0x30, CID_STABILIZATION = 0x02 };

struct Builder {
    uint8_t seq = 0;

    // Devuelve el largo escrito en `out` (>= 7 + n bytes)
    size_t packet(uint8_t *out, uint8_t did, uint8_t cid, const uint8_t *data, size_t n, bool answer = false)
    {
        uint8_t dlen = (uint8_t)(n + 1);
        uint8_t sum = (uint8_t)(did + cid + seq + dlen);
        out[0] = 0xFF;
        out[1] = (uint8_t)(0xFC | (answer ? 1 : 0) | 2);
        out[2] = did;
        out[3] = cid;
        out[4] = seq++;
        out[5] = dlen;
        for (size_t i = 0; i < n; i++) { out[6 + i] = data[i]; sum = (uint8_t)(sum + data[i]); }
        out[6 + n] = (uint8_t)~sum;
        return 7 + n;
    }

    // rumbo 0..359 (0 = hacia donde quedó calibrado), velocidad 0..255; velocidad 0 frena
    size_t roll(uint8_t *out, int speed, int heading)
    {
        if (speed < 0) speed = 0;
        if (speed > 255) speed = 255;
        heading = ((heading % 360) + 360) % 360;
        uint8_t d[4] = {(uint8_t)speed, (uint8_t)(heading >> 8), (uint8_t)(heading & 255), 1};
        return packet(out, DID_SPHERO, CID_ROLL, d, 4);
    }
    size_t rgb(uint8_t *out, uint8_t r, uint8_t g, uint8_t b)
    {
        uint8_t d[4] = {r, g, b, 0};  // flag 0 = no se guarda como color de arranque
        return packet(out, DID_SPHERO, CID_RGB, d, 4);
    }
    size_t back_led(uint8_t *out, uint8_t level)  // luz azul trasera: sirve para calibrar el rumbo
    {
        return packet(out, DID_SPHERO, CID_BACKLED, &level, 1);
    }
    size_t set_heading(uint8_t *out, int heading)  // fija el rumbo actual como `heading` (calibración)
    {
        heading = ((heading % 360) + 360) % 360;
        uint8_t d[2] = {(uint8_t)(heading >> 8), (uint8_t)(heading & 255)};
        return packet(out, DID_SPHERO, CID_HEADING, d, 2);
    }
    size_t sleep(uint8_t *out)
    {
        const uint8_t d[5] = {0, 0, 0, 0, 0};
        return packet(out, DID_CORE, CID_SLEEP, d, 5);
    }
};

// Respuesta simple por notificación: FF FF CODE SEQ DLEN DATA... CHK. Devuelve el código (0 = ok), -1 si no es válida.
inline int parse_response(const uint8_t *p, size_t n)
{
    if (n < 6 || p[0] != 0xFF || p[1] != 0xFF) return -1;
    size_t dlen = p[4];
    if (dlen == 0 || n < 5 + dlen) return -1;
    uint8_t sum = 0;
    for (size_t i = 2; i < 5 + dlen - 1; i++) sum = (uint8_t)(sum + p[i]);
    return (uint8_t)~sum == p[5 + dlen - 1] ? p[2] : -1;
}

}  // namespace sphero
