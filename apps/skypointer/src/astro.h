// Núcleo astronómico de skypointer: tiempo, precesión, paso a horizonte, Sol y
// refracción. Sin dependencias del OS ni del Arduino, para poder compilarlo y
// verificarlo en la Mac (tools/verify_astro.py).
//
// Marcos:
//   * ecuatorial J2000   -> el del catálogo (x,y,z entero, escala 32767)
//   * ecuatorial de fecha -> precesado; es el que gira con el tiempo sidéreo
//   * horizonte ENU       -> x = Este, y = Norte, z = Arriba
#pragma once
#include <math.h>

namespace sky {

constexpr double DEG = M_PI / 180.0;

struct Mat3 {
    float m[3][3];
};

// Fecha/hora UTC -> día juliano.
double julianDate(int year, int month, int day, int hh, int mm, double ss);

// Tiempo sidéreo local (rad) para una longitud este.
double localSidereal(double jdUtc, double lonDegEast);

// Precesión J2000 -> fecha (IAU 1976).
Mat3 precession(double jdUtc);

// Frame completo: pasa un vector J2000 a ENU. Se recalcula una vez por segundo.
struct HorizonFrame {
    Mat3 toHorizon;
    double lst;   // tiempo sidéreo local (rad)
    double lat;   // grados
    double lon;   // grados este

    void update(double jdUtc, double latDeg, double lonDegEast);
    // Vector J2000 en ENU; no normaliza (la entrada ya es unitaria).
    void vec(float x, float y, float z, float &east, float &north, float &up) const;
    // Azimut (rad, desde el norte hacia el este) y altura (rad) de un J2000.
    void altAz(float x, float y, float z, double &azRad, double &altRad) const;
    // Altura de un vector ENU (para filtrar antes de dibujar).
    static double altOf(float east, float north, float up) { return asin(fmax(-1.0, fmin(1.0, up))); }
};

// Refracción atmosférica (Bennett), radianes.
double refraction(double altRad);

// Sol geocéntrico aparente, ecuatorial J2000 (Meeus abreviado, error < 0.02°).
void sun(double jdUtc, double &raRad, double &decRad);

// J2000 -> unitario float, para alimentar HorizonFrame.
void toUnit(double raRad, double decRad, float &x, float &y, float &z);

double wrap2pi(double a);

} // namespace sky
