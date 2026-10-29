#include "../src/minidrone_core.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main()
{
    mini::Builder b;
    uint8_t p[32];

    size_t n = b.handshake(p);
    CHECK(n == 17 && p[0] == 4 && p[1] == 0 && p[6] == '2' && p[15] == '8' && p[16] == 0);
    n = b.takeoff(p);
    const uint8_t to[] = {2, 1, 2, 0, 1, 0};
    CHECK(n == 6 && !memcmp(p, to, 6));
    n = b.land(p);
    CHECK(p[4] == 3 && p[1] == 2);
    n = b.flip(p, mini::FLIP_LEFT);
    const uint8_t fl[] = {2, 3, 2, 4, 0, 0, 3, 0, 0, 0};
    CHECK(n == 10 && !memcmp(p, fl, 10));
    n = b.wheels(p, true);
    const uint8_t wh[] = {2, 4, 2, 1, 2, 0, 1};
    CHECK(n == 7 && !memcmp(p, wh, 7));

    n = b.pcmd(p, true, 150, -20, 0, -200);
    CHECK(n == 19 && p[6] == 1 && p[7] == 100 && (int8_t)p[8] == -20 && p[9] == 0 && (int8_t)p[10] == -100 && p[0] == 2 && p[4] == 2);
    n = b.pcmd(p, false, 0, 0, 0, 0);
    CHECK(p[1] == 1 && p[6] == 0);

    // UUID 9a66fa0b-0800-9191-11e4-012d1540cb8e
    const uint8_t u[16] = {0x8e, 0xcb, 0x40, 0x15, 0x2d, 0x01, 0xe4, 0x11, 0x91, 0x91, 0x00, 0x08, 0x0b, 0xfa, 0x66, 0x9a};
    CHECK(mini::uuid_is(u, mini::CHR_CMD) && !mini::uuid_is(u, mini::CHR_PCMD));

    const uint8_t st[] = {4, 1, 0, 3, 3, 0, 2, 0};
    CHECK(mini::parse_state(st, sizeof(st)) == mini::HOVERING && mini::parse_battery(st, 3) == 0);
    CHECK(mini::is_minidrone("RS_W123456") && !mini::is_minidrone("BB-1"));
    printf(fails ? "FAILED\n" : "PASS\n");
    return fails;
}
