#include "sat.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const double D2R = M_PI / 180.0;
static const double WGS84_A = 6378.137;
static const double WGS84_F = 1.0 / 298.257223563;

static double col(const char *line, int from, int len)
{
    char buf[32];
    if (len > 30) len = 30;
    memcpy(buf, line + from - 1, len);
    buf[len] = 0;
    return atof(buf);
}

// Campo de la línea 1 con exponente implícito: " 12026-4" -> 0.12026e-4
static double impliedExp(const char *line, int from)
{
    char f[9];
    memcpy(f, line + from - 1, 8);
    f[8] = 0;
    double mant = 0;
    for (int i = 1; i <= 5; ++i)
        if (f[i] >= '0' && f[i] <= '9') mant = mant * 10 + (f[i] - '0');
    mant /= 1e5;
    if (f[0] == '-') mant = -mant;
    int exp = (f[6] == '-' ? -1 : 1) * (f[7] - '0');
    return mant * pow(10.0, exp);
}

bool satFromTle(const char *line1, const char *line2, Sat &out, const char *name)
{
    memset(&out, 0, sizeof(out));
    if (!line1 || !line2 || line1[0] != '1' || line2[0] != '2') return false;

    // Época: YYDDD.DDDDDDDD, dos dígitos de año y día del año con fracción.
    char ebuf[16];
    memcpy(ebuf, line1 + 18, 14);
    ebuf[14] = 0;
    int yy = (ebuf[0] - '0') * 10 + (ebuf[1] - '0');  // solo los dos dígitos del año
    double doy = atof(ebuf + 2);
    int year = (yy < 57) ? 2000 + yy : 1900 + yy;

    // JD de las 00:00 UT del 1 de enero de ese año (algoritmo de Hinnant).
    int y = year, m = 1, d = 1;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doyJ = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doyJ;
    long jdn = (long)era * 146097 + (long)doe - 719468 + 2440588; // 1970-01-01 = 2440588
    double jdJan1 = (double)jdn - 0.5;

    out.epochJd = jdJan1 + (doy - 1.0);

    memcpy(out.rec.satid, line2 + 2, 5);
    out.rec.satid[5] = 0;
    out.rec.whichconst = wgs72;
    out.rec.jdsatepoch = floor(out.epochJd);
    out.rec.jdsatepochF = out.epochJd - out.rec.jdsatepoch;
    out.rec.bstar = impliedExp(line1, 54);
    out.rec.inclo = col(line2, 9, 8) * D2R;
    out.rec.nodeo = col(line2, 18, 8) * D2R;
    double ecc = 0;
    for (int i = 0; i < 7; ++i) {
        char c = line2[26 + i];
        ecc = ecc * 10 + (c >= '0' && c <= '9' ? c - '0' : 0);
    }
    out.rec.ecco = ecc / 1e7;
    out.rec.argpo = col(line2, 35, 8) * D2R;
    out.rec.mo = col(line2, 44, 8) * D2R;
    out.rec.no_kozai = col(line2, 53, 11) * 2.0 * M_PI / 1440.0;

    if (name && name[0]) snprintf(out.name, sizeof(out.name), "%s", name);
    else snprintf(out.name, sizeof(out.name), "%s", out.rec.satid);

    out.rec.error = 0;
    if (!sgp4init('i', &out.rec)) {
        out.valid = false;
        return false;
    }
    out.valid = true;
    return true;
}

// Geodésico -> ECEF (WGS84)
static void geodeticToEcef(double latDeg, double lonDeg, double hKm, double &x, double &y, double &z)
{
    double lat = latDeg * D2R, lon = lonDeg * D2R;
    double e2 = WGS84_F * (2 - WGS84_F);
    double n = WGS84_A / sqrt(1 - e2 * sin(lat) * sin(lat));
    x = (n + hKm) * cos(lat) * cos(lon);
    y = (n + hKm) * cos(lat) * sin(lon);
    z = (n * (1 - e2) + hKm) * sin(lat);
}

bool satAzAlt(const Sat &s, double jdUtc, double latDeg, double lonDegEast,
              double &azRad, double &altRad, double &rangeKm)
{
    if (!s.valid) return false;
    double tsince = (jdUtc - s.epochJd) * 1440.0;
    double r[3], v[3];
    if (!sgp4((ElsetRec *)&s.rec, tsince, r, v)) return false;

    double theta = gstime(jdUtc);
    double c = cos(theta), sn = sin(theta);
    double xe = c * r[0] + sn * r[1];
    double ye = -sn * r[0] + c * r[1];
    double ze = r[2];

    double ox, oy, oz;
    geodeticToEcef(latDeg, lonDegEast, 0.0, ox, oy, oz);
    double dx = xe - ox, dy = ye - oy, dz = ze - oz;

    double lat = latDeg * D2R, lon = lonDegEast * D2R;
    double east = -sin(lon) * dx + cos(lon) * dy;
    double north = -sin(lat) * cos(lon) * dx - sin(lat) * sin(lon) * dy + cos(lat) * dz;
    double up = cos(lat) * cos(lon) * dx + cos(lat) * sin(lon) * dy + sin(lat) * dz;

    rangeKm = sqrt(dx * dx + dy * dy + dz * dz);
    altRad = asin(up / rangeKm);
    azRad = atan2(east, north);
    if (azRad < 0) azRad += 2 * M_PI;
    return true;
}
