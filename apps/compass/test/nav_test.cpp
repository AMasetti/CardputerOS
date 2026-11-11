// g++ -std=c++17 -Iapps/compass/src apps/compass/test/nav_test.cpp -o /tmp/t && /tmp/t
#include "nav.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    // Rosario (Monumento a la Bandera) -> Buenos Aires (Obelisco): ~278 km hacia el SE
    int32_t ros_lat = -329476000, ros_lon = -606307000, bue_lat = -346037000, bue_lon = -583816000;
    double d = nav_distance_m(ros_lat, ros_lon, bue_lat, bue_lon);
    double b = nav_bearing_deg(ros_lat, ros_lon, bue_lat, bue_lon);
    printf("Rosario -> Buenos Aires: %.1f km, %.1f°\n", d / 1000, b);
    CHECK(d > 270000 && d < 290000);
    CHECK(b > 125 && b < 140);
    // los cuatro puntos cardinales
    CHECK(fabs(nav_bearing_deg(0, 0, 10000000, 0) - 0) < 0.01);
    CHECK(fabs(nav_bearing_deg(0, 0, 0, 10000000) - 90) < 0.01);
    CHECK(fabs(nav_bearing_deg(0, 0, -10000000, 0) - 180) < 0.01);
    CHECK(fabs(nav_bearing_deg(0, 0, 0, -10000000) - 270) < 0.01);
    // 1° de latitud ~ 111.2 km; mismo punto: 0
    CHECK(fabs(nav_distance_m(0, 0, 10000000, 0) - 111195) < 50);
    CHECK(nav_distance_m(ros_lat, ros_lon, ros_lat, ros_lon) == 0);
    // precisión: 15 bits ~ 700 m (la app Meshtastic dice ±729 m)
    double p15 = nav_precision_m(15);
    printf("15 bits: ±%.0f m, 14 bits: ±%.0f m\n", p15, nav_precision_m(14));
    CHECK(p15 > 650 && p15 < 800);
    CHECK(nav_precision_m(32) == 0);

    char buf[32];
    nav_format_distance(850.4, buf, sizeof(buf)); CHECK(!strcmp(buf, "850 m"));
    nav_format_distance(1234, buf, sizeof(buf)); CHECK(!strcmp(buf, "1.23 km"));
    nav_format_distance(27800, buf, sizeof(buf)); CHECK(!strcmp(buf, "27.8 km"));
    nav_format_distance(278400, buf, sizeof(buf)); CHECK(!strcmp(buf, "278 km"));

    const char *csv = "# mis puntos\nAuto, -32.9476, -60.6307\n\nCampamento,-32.95,-60.62 # orilla\nmal,abc,def\nlejos,95,0\n  Casa ,-34.6037,-58.3816\r\n";
    Target t[8];
    int n = nav_parse_points(csv, t, 8);
    CHECK(n == 3);
    CHECK(!strcmp(t[0].name, "Auto") && t[0].lat == -329476000 && t[0].lon == -606307000);
    CHECK(!strcmp(t[1].name, "Campamento"));
    CHECK(!strcmp(t[2].name, "Casa") && t[2].lon == -583816000);
    char line[96];
    nav_format_point(t[0], line, sizeof(line));
    Target back[1];
    CHECK(nav_parse_points(line, back, 1) == 1 && back[0].lat == t[0].lat && back[0].lon == t[0].lon);

    printf(fails ? "%d FAILED\n" : "all nav tests passed\n", fails);
    return fails ? 1 : 0;
}
