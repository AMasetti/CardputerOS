// Satélites: TLE -> SGP4 -> azimut/altura desde el observador.
// El propagador es el SGP4 de Vallado portado a C por aholinch (dominio público).
#pragma once
#include <stdint.h>

extern "C" {
#include "vendor/SGP4.h"
}

struct Sat {
    ElsetRec rec;
    double epochJd;
    char name[24];
    bool valid;
};

bool satFromTle(const char *line1, const char *line2, Sat &out, const char *name = "");
bool satAzAlt(const Sat &s, double jdUtc, double latDeg, double lonDegEast,
              double &azRad, double &altRad, double &rangeKm);
