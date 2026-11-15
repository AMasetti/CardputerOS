// Núcleo astronómico: tiempo, precesión, paso a horizonte, Sol y refracción.
// Todo dobles en la inicialización y flotantes simples en el lazo por estrella,
// porque el ESP32-S3 solo tiene FPU de simple precisión.
#include "astro.h"

namespace sky {

double julianDate(int year, int month, int day, int hh, int mm, double ss)
{
    // Algoritmo de Vallado/Duffett-Smith, válido para todo el calendario gregoriano.
    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    int a = year / 100;
    int b = 2 - a + a / 4;
    return floor(365.25 * (year + 4716)) + floor(30.6001 * (month + 1)) + day + b - 1524.5 +
           (hh + mm / 60.0 + ss / 3600.0) / 24.0;
}

double wrap2pi(double a)
{
    a = fmod(a, 2 * M_PI);
    if (a < 0) a += 2 * M_PI;
    return a;
}

double localSidereal(double jdUtc, double lonDegEast)
{
    double tut1 = (jdUtc - 2451545.0) / 36525.0;
    double sec = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 +
                 (876600.0 * 3600 + 8640184.812866) * tut1 + 67310.54841;
    return wrap2pi(sec * DEG / 240.0 + lonDegEast * DEG);
}

static void mul(const float a[3][3], const float b[3][3], float out[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

Mat3 precession(double jdUtc)
{
    double t = (jdUtc - 2451545.0) / 36525.0;
    double zeta = (2306.2181 * t + 0.30188 * t * t + 0.017998 * t * t * t) / 3600.0 * DEG;
    double z = (2306.2181 * t + 1.09468 * t * t + 0.018203 * t * t * t) / 3600.0 * DEG;
    double theta = (2004.3109 * t - 0.42665 * t * t - 0.041833 * t * t * t) / 3600.0 * DEG;

    Mat3 rz1 = {{{(float)cos(zeta), (float)-sin(zeta), 0}, {(float)sin(zeta), (float)cos(zeta), 0}, {0, 0, 1}}};
    Mat3 ry = {{{(float)cos(theta), 0, (float)-sin(theta)}, {0, 1, 0}, {(float)sin(theta), 0, (float)cos(theta)}}};
    Mat3 rz2 = {{{(float)cos(z), (float)-sin(z), 0}, {(float)sin(z), (float)cos(z), 0}, {0, 0, 1}}};
    Mat3 tmp, out;
    mul(ry.m, rz1.m, tmp.m); // P = Rz(-z) * Ry(theta) * Rz(-zeta)
    mul(rz2.m, tmp.m, out.m);
    return out;
}

void HorizonFrame::update(double jdUtc, double latDeg, double lonDegEast)
{
    lat = latDeg;
    lon = lonDegEast;
    lst = localSidereal(jdUtc, lonDegEast);

    double phi = latDeg * DEG;
    double c = cos(lst), s = sin(lst);
    double sp = sin(phi), cp = cos(phi);

    // fila 0 = Este, fila 1 = Norte, fila 2 = Arriba
    float h[3][3] = {
        {(float)-s, (float)c, 0.0f},
        {(float)(-sp * c), (float)(-sp * s), (float)cp},
        {(float)(cp * c), (float)(cp * s), (float)sp},
    };
    Mat3 p = precession(jdUtc);
    mul(h, p.m, toHorizon.m);
}

void HorizonFrame::vec(float x, float y, float z, float &east, float &north, float &up) const
{
    east = toHorizon.m[0][0] * x + toHorizon.m[0][1] * y + toHorizon.m[0][2] * z;
    north = toHorizon.m[1][0] * x + toHorizon.m[1][1] * y + toHorizon.m[1][2] * z;
    up = toHorizon.m[2][0] * x + toHorizon.m[2][1] * y + toHorizon.m[2][2] * z;
}

void HorizonFrame::altAz(float x, float y, float z, double &azRad, double &altRad) const
{
    float e, n, u;
    vec(x, y, z, e, n, u);
    altRad = altOf(e, n, u);
    azRad = wrap2pi(atan2((double)e, (double)n));
}

double refraction(double altRad)
{
    double h = altRad / DEG;
    if (h < -1.0) return 0;
    return 1.02 / tan((h + 10.3 / (h + 5.11)) * DEG) / 60.0 * DEG;
}

void toUnit(double raRad, double decRad, float &x, float &y, float &z)
{
    x = (float)(cos(decRad) * cos(raRad));
    y = (float)(cos(decRad) * sin(raRad));
    z = (float)sin(decRad);
}

void sun(double jdUtc, double &raRad, double &decRad)
{
    // Meeus, Astronomical Algorithms, cap. 25 (precisión 0.01°).
    double n = jdUtc - 2451545.0;
    double L = (280.460 + 0.9856474 * n) * DEG;
    double g = (357.528 + 0.9856003 * n) * DEG;
    double lambda = L + (1.915 * sin(g) + 0.020 * sin(2 * g)) * DEG;
    double eps = (23.439 - 0.0000004 * n) * DEG;

    // Las coordenadas salen de la fecha, y el resto del firmware trabaja en J2000.
    double ra = atan2(cos(eps) * sin(lambda), cos(lambda));
    double dec = asin(sin(eps) * sin(lambda));

    float x, y, z;
    toUnit(ra, dec, x, y, z);
    Mat3 p = precession(jdUtc);
    float xj = p.m[0][0] * x + p.m[1][0] * y + p.m[2][0] * z; // transpuesta
    float yj = p.m[0][1] * x + p.m[1][1] * y + p.m[2][1] * z;
    float zj = p.m[0][2] * x + p.m[1][2] * y + p.m[2][2] * z;
    decRad = asin(fmax(-1.0, fmin(1.0, (double)zj)));
    raRad = wrap2pi(atan2((double)yj, (double)xj));
}

} // namespace sky
