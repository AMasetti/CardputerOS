// GNSS receiver of the Cap LoRa-1262 (NMEA over the OS serial port), shared by the Meshtastic apps.
// Needs <M5Cardputer.h> (millis, cpos_api) included first.
#pragma once
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct Gnss {
    bool up = false, fix = false;
    int32_t lat = 0, lon = 0; // grados * 1e7
    int32_t alt = 0;
    float speed_kmh = 0;      // velocidad sobre el suelo
    float course = -1;        // rumbo de marcha (° desde el norte verdadero), -1 sin dato
    float hdop = 0;
    uint8_t sats = 0;
    uint32_t unix = 0, unix_at = 0; // hora UTC y millis() en que se leyó
    uint32_t fix_at = 0;
    char line[100];
    uint8_t n = 0;
} G;

static int32_t nmeaCoord(const char *v, const char *hemi) {
    // ddmm.mmmm / dddmm.mmmm -> grados * 1e7
    if (!*v) return 0;
    double x = atof(v);
    int deg = (int)(x / 100);
    double minutes = x - deg * 100;
    double d = deg + minutes / 60.0;
    if (*hemi == 'S' || *hemi == 'W') d = -d;
    return (int32_t)lround(d * 1e7);
}

static uint32_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468);
}

static void nmeaLine(char *s) {
    // checksum
    char *star = strchr(s, '*');
    if (!star || s[0] != '$') return;
    uint8_t cs = 0;
    for (char *p = s + 1; p < star; p++) cs ^= (uint8_t)*p;
    if (strtoul(star + 1, nullptr, 16) != cs) return;
    *star = 0;
    char *f[20];
    int nf = 0;
    for (char *p = s; nf < 20;) {
        f[nf++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    const char *type = f[0] + 3; // $GPRMC / $GNRMC ...
    if (!strcmp(type, "RMC") && nf >= 10) {
        bool valid = f[2][0] == 'A';
        if (valid) {
            G.speed_kmh = f[7][0] ? (float)atof(f[7]) * 1.852f : 0; // nudos
            G.course = f[8][0] ? (float)atof(f[8]) : -1;
            G.lat = nmeaCoord(f[3], f[4]);
            G.lon = nmeaCoord(f[5], f[6]);
            G.fix = true;
            G.fix_at = millis();
        }
        if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
            int hh = (f[1][0] - '0') * 10 + f[1][1] - '0', mi = (f[1][2] - '0') * 10 + f[1][3] - '0',
                ss = (f[1][4] - '0') * 10 + f[1][5] - '0';
            int dd = (f[9][0] - '0') * 10 + f[9][1] - '0', mo = (f[9][2] - '0') * 10 + f[9][3] - '0',
                yy = 2000 + (f[9][4] - '0') * 10 + f[9][5] - '0';
            if (yy >= 2024 && mo >= 1 && mo <= 12) {
                G.unix = daysFromCivil(yy, mo, dd) * 86400u + hh * 3600 + mi * 60 + ss;
                G.unix_at = millis();
            }
        }
    } else if (!strcmp(type, "GGA") && nf >= 10) {
        G.sats = (uint8_t)atoi(f[7]);
        G.hdop = (float)atof(f[8]);
        if (atoi(f[6]) > 0) G.alt = (int32_t)lround(atof(f[9]));
    }
}

static void pollGnss() {
    if (!G.up) return;
    uint8_t buf[128];
    int n;
    while ((n = cpos_api->lora.gnss_read(buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (G.n) { G.line[G.n] = 0; nmeaLine(G.line); }
                G.n = 0;
            } else if (G.n < sizeof(G.line) - 1) {
                G.line[G.n++] = c;
            } else {
                G.n = 0;
            }
        }
    }
    if (G.fix && millis() - G.fix_at > 10000) G.fix = false; // sin datos válidos hace rato
}

static void setGps(bool on) {
    if (on && !G.up) G.up = cpos_api->lora.gnss_begin(115200) == 0;
    if (!on && G.up) { cpos_api->lora.gnss_end(); G.up = false; G.fix = false; }
}

// hora UTC: la del GPS, o la que haya puesto el teléfono (gnss_set_time), o 0
static uint32_t g_time_base = 0, g_time_at = 0;
static void gnss_set_time(uint32_t unix_time) { g_time_base = unix_time; g_time_at = millis(); }
static uint32_t nowUnix() {
    if (G.unix) return G.unix + (millis() - G.unix_at) / 1000;
    return g_time_base ? g_time_base + (millis() - g_time_at) / 1000 : 0;
}

