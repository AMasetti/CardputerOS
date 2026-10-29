#include "../src/sphero2_core.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main()
{
    sphero2::Builder b;
    uint8_t p[96], body[64];

    // wake sin destino: 8D 0A 13 0D 00 D5 D8 (vector conocido del R2-D2)
    size_t n = b.wake(p);
    const uint8_t wake[] = {0x8D, 0x0A, 0x13, 0x0D, 0x00, 0xD5, 0xD8};
    CHECK(n == 7 && !memcmp(p, wake, 7));

    // manejo: ida y vuelta por unpack
    n = b.drive(p, 100, 270, 0, 0);
    int bl = sphero2::unpack(p, n, body, sizeof(body));
    CHECK(bl == 8);
    CHECK(body[0] == 0x0A && body[1] == 0x16 && body[2] == 0x07 && body[3] == 1);
    CHECK(body[4] == 100 && body[5] == 0x01 && body[6] == 0x0E && body[7] == 0);

    // con destino (BOLT): flags 0x3A, tid 0x12, sid 1
    n = b.drive(p, 0, 0, 0, sphero2::TID_SECONDARY);
    bl = sphero2::unpack(p, n, body, sizeof(body));
    CHECK(bl > 0 && body[0] == 0x3A && body[1] == 0x12 && body[2] == 0x01 && body[3] == 0x16);

    // escape: un dato 0x8D / 0xD8 / 0xAB no aparece crudo dentro del paquete
    const uint8_t evil[3] = {0x8D, 0xD8, 0xAB};
    n = b.packet(p, 0x1A, 0x07, 0, evil, 3);
    for (size_t i = 1; i + 1 < n; i++) CHECK(p[i] != 0x8D && p[i] != 0xD8);
    bl = sphero2::unpack(p, n, body, sizeof(body));
    CHECK(bl == 7 && body[4] == 0x8D && body[5] == 0xD8 && body[6] == 0xAB);

    // checksum dañado
    n = b.wake(p);
    p[3] ^= 1;
    CHECK(sphero2::unpack(p, n, body, sizeof(body)) == -1);

    // secuencia envuelve en 0xFF
    b.seq = 254;
    b.wake(p);
    CHECK(b.seq == 0);

    // cabeza 90.0 = 0x42B40000 big endian
    n = b.head(p, 90.0f);
    bl = sphero2::unpack(p, n, body, sizeof(body));
    CHECK(body[2] == 15 && body[4] == 0x42 && body[5] == 0xB4 && body[6] == 0 && body[7] == 0);

    // luces: máscara 32 bits big endian + valores
    uint8_t v[12];
    size_t nv = sphero2::color_vals(sphero2::R2D2, v, 1, 2, 3);
    n = b.leds(p, sphero2::color_mask(sphero2::R2D2), v, nv);
    bl = sphero2::unpack(p, n, body, sizeof(body));
    CHECK(body[2] == 26 && body[4] == 0 && body[7] == 0x77 && body[8] == 1 && body[13] == 3);

    CHECK(sphero2::kind_of("D2-1A2B") == sphero2::R2D2 && sphero2::kind_of("GB-9999") == sphero2::BB9E && sphero2::kind_of("BB-1") == -1);
    printf(fails ? "FAILED\n" : "PASS\n");
    return fails;
}
