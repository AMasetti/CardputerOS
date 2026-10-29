// Protocolo de los minidrones Parrot por BLE (Rolling Spider): paquetes y UUIDs, sin dependencias.
//
// Los comandos van por la característica FA0B (con ack), el manejo (PCMD) por FA0A (sin ack) y el
// paro de emergencia por FA0C. Las notificaciones llegan por FB0E (estado de vuelo) y FB0F (batería).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace mini {

// 9a66XXXX-0800-9191-11e4-012d1540cb8e en little endian: los 4 hex de XXXX van en [13]:[12]
inline bool uuid_is(const uint8_t u[16], uint16_t id)
{
    static const uint8_t tail[12] = {0x8e, 0xcb, 0x40, 0x15, 0x2d, 0x01, 0xe4, 0x11, 0x91, 0x91, 0x00, 0x08};
    return !memcmp(u, tail, 12) && u[12] == (id & 255) && u[13] == (id >> 8) && u[14] == 0x66 && u[15] == 0x9a;
}
enum : uint16_t { CHR_PCMD = 0xFA0A, CHR_CMD = 0xFA0B, CHR_EMERGENCY = 0xFA0C, CHR_STATE = 0xFB0E, CHR_BATTERY = 0xFB0F };
// características que se suscriben al conectar (el robot no responde sin esto)
static const uint16_t NOTIFY_IDS[] = {0xFB0F, 0xFB0E, 0xFB1B, 0xFB1C, 0xFD22, 0xFD23, 0xFD24, 0xFD52, 0xFD53, 0xFD54};

inline bool is_minidrone(const char *name) { return !strncmp(name, "RS_", 3); }

enum State : uint8_t { LANDED = 0, TAKING_OFF = 1, HOVERING = 2, FLYING = 3, LANDING = 4, EMERGENCY = 5 };
enum Flip : uint8_t { FLIP_FRONT = 0, FLIP_BACK = 1, FLIP_RIGHT = 2, FLIP_LEFT = 3 };

struct Builder {
    uint8_t cmd_seq = 0, pcmd_seq = 0;

    size_t handshake(uint8_t *out)  // fija la fecha: sin esto algunos firmwares ignoran los comandos
    {
        static const uint8_t d[] = {0x32, 0x30, 0x31, 0x34, 0x2D, 0x31, 0x30, 0x2D, 0x32, 0x38, 0x00};  // "2014-10-28"
        out[0] = 0x04; out[1] = cmd_seq++; out[2] = 0x00; out[3] = 0x04; out[4] = 0x01; out[5] = 0x00;
        memcpy(out + 6, d, sizeof(d));
        return 6 + sizeof(d);
    }
    size_t cmd(uint8_t *out, uint8_t cls, uint8_t id, const uint8_t *args = nullptr, size_t n = 0)  // proyecto 2 (minidrone)
    {
        out[0] = 0x02; out[1] = cmd_seq++; out[2] = 0x02; out[3] = cls; out[4] = id; out[5] = 0x00;
        for (size_t i = 0; i < n; i++) out[6 + i] = args[i];
        return 6 + n;
    }
    size_t flat_trim(uint8_t *out) { return cmd(out, 0, 0); }
    size_t takeoff(uint8_t *out) { return cmd(out, 0, 1); }
    size_t land(uint8_t *out) { return cmd(out, 0, 3); }
    size_t emergency(uint8_t *out) { return cmd(out, 0, 4); }  // se escribe en FA0C
    size_t flip(uint8_t *out, Flip f)
    {
        const uint8_t a[4] = {f, 0, 0, 0};
        return cmd(out, 4, 0, a, 4);
    }
    size_t wheels(uint8_t *out, bool on)  // ruedas (cuerpo con ruedas) puestas / sacadas
    {
        uint8_t a = on;
        return cmd(out, 1, 2, &a, 1);
    }
    size_t picture(uint8_t *out)
    {
        const uint8_t a = 0;
        return cmd(out, 6, 0, &a, 1);
    }

    // PCMD de 19 bytes: roll (lateral), pitch (adelante/atrás), yaw (giro), gaz (altura), todos -100..100.
    // moving = 0 hace que el dron se quede en su lugar aunque los ejes no sean cero.
    size_t pcmd(uint8_t *out, bool moving, int roll, int pitch, int yaw, int gaz)
    {
        auto c = [](int v) { return (uint8_t)(int8_t)(v < -100 ? -100 : (v > 100 ? 100 : v)); };
        memset(out, 0, 19);
        out[0] = 0x02; out[1] = pcmd_seq++; out[2] = 0x02; out[3] = 0x00; out[4] = 0x02; out[5] = 0x00;
        out[6] = moving;
        out[7] = c(roll); out[8] = c(pitch); out[9] = c(yaw); out[10] = c(gaz);
        return 19;
    }
};

// FB0E: el byte 6 es el estado; FB0F: el último byte es la batería (%)
inline int parse_state(const uint8_t *p, size_t n) { return n > 6 ? p[6] : -1; }
inline int parse_battery(const uint8_t *p, size_t n) { return n ? p[n - 1] : -1; }

}  // namespace mini
