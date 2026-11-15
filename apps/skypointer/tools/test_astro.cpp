// Test de host: imprime alt/az calculados por la app para comparar contra PyEphem
// (tools/verify_astro.py). No forma parte del build.
#include "../src/astro.h"
#include "../src/sat.h"
#include <cstdio>
#include <cstdlib>

using namespace sky;

static void printAltAz(const char *name, float x, float y, float z, const HorizonFrame &f)
{
    double az, alt;
    f.altAz(x, y, z, az, alt);
    printf("%s\t%.6f\t%.6f\n", name, wrap2pi(az) / DEG, alt / DEG);
}

int main(int argc, char **argv)
{
    // uso: test_astro <lat> <lon> <jd> [tle1] [tle2]
    if (argc < 4) return 2;
    double lat = atof(argv[1]), lon = atof(argv[2]), jd = atof(argv[3]);
    HorizonFrame f;
    f.update(jd, lat, lon);

    // Estrellas de referencia: (RA J2000 deg, Dec J2000 deg)
    struct { const char *name; double ra, dec; } stars[] = {
        {"Sirius", 101.287155, -16.716116},
        {"Vega", 279.234735, 38.783689},
        {"Arcturus", 213.915300, 19.182409},
        {"Canopus", 95.987958, -52.695661},
        {"Polaris", 37.954560, 89.264109},
        {"Fomalhaut", 344.412693, -29.622237},
        {"Achernar", 24.428527, -57.236753},
    };
    for (auto &s : stars) {
        float x, y, z;
        toUnit(s.ra * DEG, s.dec * DEG, x, y, z);
        printAltAz(s.name, x, y, z, f);
    }

    double ra, dec, alt;
    float x, y, z;
    sun(jd, ra, dec);
    toUnit(ra, dec, x, y, z);
    printAltAz("Sun", x, y, z, f);

    if (argc >= 6) {
        Sat el;
        if (satFromTle(argv[4], argv[5], el, "ISS")) {
            double az, range;
            if (satAzAlt(el, jd, lat, lon, az, alt, range)) {
                printf("ISS\t%.6f\t%.6f\n", az / DEG, alt / DEG);
                printf("ISSRange\t%.3f\t0\n", range);
            } else {
                printf("ISS\tERROR\t0\n");
            }
        } else {
            printf("ISS\tPARSE_ERROR\t0\n");
        }
    }

    printf("LST\t%.6f\t0\n", f.lst / DEG);
    return 0;
}
