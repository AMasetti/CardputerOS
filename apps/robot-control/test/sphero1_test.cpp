#include "../src/sphero1_core.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main()
{
    sphero1::Builder b;
    uint8_t p[32];

    // roll 0x40 rumbo 270: FF FE 02 30 00 05 40 01 0E 01 chk
    size_t n = b.roll(p, 0x40, 270);
    CHECK(n == 11);
    const uint8_t want[] = {0xFF, 0xFE, 0x02, 0x30, 0x00, 0x05, 0x40, 0x01, 0x0E, 0x01, 0};
    CHECK(memcmp(p, want, 10) == 0);
    uint8_t sum = 0x02 + 0x30 + 0x00 + 0x05 + 0x40 + 0x01 + 0x0E + 0x01;
    CHECK(p[10] == (uint8_t)~sum);

    // secuencia aumenta, rumbo se normaliza, velocidad se acota
    n = b.roll(p, 999, -90);
    CHECK(p[4] == 1 && p[6] == 255 && p[7] == 0x01 && p[8] == 0x0E);

    // el checksum de cualquier paquete cierra: suma de DID..CHK = 0xFF
    n = b.rgb(p, 255, 0, 128);
    uint8_t t = 0;
    for (size_t i = 2; i < n; i++) t += p[i];
    CHECK(t == 0xFF);
    n = b.sleep(p);
    t = 0;
    for (size_t i = 2; i < n; i++) t += p[i];
    CHECK(t == 0xFF && p[2] == 0x00 && p[3] == 0x22 && p[5] == 6);

    // respuesta simple válida / dañada
    const uint8_t resp[] = {0xFF, 0xFF, 0x00, 0x07, 0x01, (uint8_t)~(0x00 + 0x07 + 0x01)};
    CHECK(sphero1::parse_response(resp, sizeof(resp)) == 0);
    uint8_t bad[6];
    memcpy(bad, resp, 6);
    bad[5] ^= 1;
    CHECK(sphero1::parse_response(bad, 6) == -1);

    // UUIDs: 22bb746f-2ba1-7554-2d6f-726568705327 en little endian
    const uint8_t u[16] = {0x27, 0x53, 0x70, 0x68, 0x65, 0x72, 0x6f, 0x2d, 0x54, 0x75, 0xa1, 0x2b, 0x6f, 0x74, 0xbb, 0x22};
    CHECK(sphero1::uuid_is(u, sphero1::CHR_CMD));
    CHECK(!sphero1::uuid_is(u, sphero1::CHR_WAKE));

    printf(fails ? "FAILED\n" : "PASS\n");
    return fails;
}
