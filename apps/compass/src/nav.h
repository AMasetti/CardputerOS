// Navegación para la brújula: distancia y rumbo entre coordenadas, y los puntos guardados en la SD.
// Sin hardware: test/nav_test.cpp lo prueba en la PC.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum TargetKind : uint8_t { TARGET_POINT, TARGET_NODE };

struct Target {
    char name[24];
    int32_t lat, lon;       // grados * 1e7
    uint8_t kind;           // TargetKind
    uint8_t precision;      // bits de precisión de la posición (32 = exacta; los nodos públicos mandan 15)
};

static constexpr double NAV_R = 6371000.0; // radio medio de la Tierra (m)

// distancia sobre la esfera (m)
inline double nav_distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2) {
    double a1 = lat1 * 1e-7 * M_PI / 180, a2 = lat2 * 1e-7 * M_PI / 180;
    double dl = (lon2 - lon1) * 1e-7 * M_PI / 180;
    double x = sin((a2 - a1) / 2), y = sin(dl / 2);
    double h = x * x + cos(a1) * cos(a2) * y * y;
    return 2 * NAV_R * asin(sqrt(h < 1 ? h : 1));
}

// rumbo inicial de 1 a 2, en grados desde el norte verdadero [0, 360)
inline double nav_bearing_deg(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2) {
    double a1 = lat1 * 1e-7 * M_PI / 180, a2 = lat2 * 1e-7 * M_PI / 180;
    double dl = (lon2 - lon1) * 1e-7 * M_PI / 180;
    double b = atan2(sin(dl) * cos(a2), cos(a1) * sin(a2) - sin(a1) * cos(a2) * cos(dl)) * 180 / M_PI;
    b = fmod(b + 360, 360);
    return b >= 360 ? 0 : b;
}

// incertidumbre aproximada (m) de una posición con `bits` de precisión (como muestra la app Meshtastic)
inline double nav_precision_m(uint8_t bits) {
    if (bits == 0 || bits >= 32) return 0;
    // 2^(32-bits) unidades de 1e-7° en latitud, la mitad del cuadrado
    return (double)(1ULL << (32 - bits)) * 1e-7 * 111320.0 / 2;
}

inline void nav_format_distance(double m, char *out, size_t n) {
    if (m < 1000) snprintf(out, n, "%d m", (int)lround(m));
    else if (m < 10000) snprintf(out, n, "%.2f km", m / 1000);
    else if (m < 100000) snprintf(out, n, "%.1f km", m / 1000);
    else snprintf(out, n, "%d km", (int)lround(m / 1000));
}

// "nombre,lat,lon" por línea, grados decimales; '#' empieza un comentario. Devuelve cuántos leyó.
inline int nav_parse_points(const char *text, Target *out, int max) {
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[96];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (eol ? 1 : 0);
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *c1 = strchr(line, ',');
        if (!c1) continue;
        char *c2 = strchr(c1 + 1, ',');
        if (!c2) continue;
        *c1 = *c2 = 0;
        char *end1, *end2;
        double lat = strtod(c1 + 1, &end1), lon = strtod(c2 + 1, &end2);
        if (end1 == c1 + 1 || end2 == c2 + 1 || lat < -90 || lat > 90 || lon < -180 || lon > 180) continue;
        char *name = line;
        while (*name == ' ') name++;
        size_t nl = strlen(name);
        while (nl && (name[nl - 1] == ' ' || name[nl - 1] == '\r')) name[--nl] = 0;
        Target &t = out[n++];
        memset(&t, 0, sizeof(t));
        snprintf(t.name, sizeof(t.name), "%.23s", nl ? name : "Punto");
        t.lat = (int32_t)lround(lat * 1e7);
        t.lon = (int32_t)lround(lon * 1e7);
        t.kind = TARGET_POINT;
        t.precision = 32;
    }
    return n;
}

inline int nav_format_point(const Target &t, char *out, size_t n) {
    return snprintf(out, n, "%s,%.7f,%.7f\n", t.name, t.lat * 1e-7, t.lon * 1e-7);
}
