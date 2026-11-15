// Buscador de cielo para M5Stack Cardputer-Adv
//
// Sostenés el Cardputer como una cámara: la pantalla es una ventana al cielo y sobre lo que apuntás se
// dibujan estrellas, constelaciones, el Sol, la ISS y el tráfico aéreo, en su posición real (azimut y
// altura) calculada con la posición GPS, la hora UTC y la actitud del equipo (IMU).
//
// El BMI270 no tiene magnetómetro: la inclinación sale de la gravedad y no deriva, pero el rumbo hay
// que fijarlo a mano (n: borde de arriba al norte, enter: apuntar al Sol o a la estrella que propone
// la app) y deriva de a poco con el giróscopo.
//
//   tab        capa: Estrellas / Satélites / Aéreos / Todo
//   n          fijar el norte: el borde de arriba (o la parte de atrás, si lo tenés parado) apunta al norte
//   enter      fijar el rumbo apuntando a la referencia que propone la app (el Sol o una estrella)
//   ,  .       mover el cielo 1° a la izquierda / derecha (se repite si la dejás apretada)
//   s          sincronizar por WiFi: reloj (NTP) y TLE de la ISS (Celestrak, HTTP)
//   c          recalibrar el giróscopo (dejalo quieto)
//   x          cambiar la orientación del sensor si los ejes no coinciden
//
// Solo usa la ABI del OS (cpos_api): pantalla, IMU, GNSS del Cap LoRa-1262, red, prefs y SD.
// La astronomía está en astro.cpp / sat.cpp (verificada contra PyEphem, tools/verify_astro.py).

#include <Arduino.h>
#include <cpos/abi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "gnss.h" // lector NMEA compartido con Meshtastic (apps/meshtastic/src)
#include "astro.h"
#include "sat.h"
#include "catalog.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#define GFX (cpos_api->gfx)
#define NET (cpos_api->net)

static constexpr float RAD = M_PI / 180.0f;
static constexpr float DEGF = 180.0f / M_PI;
static constexpr int W = 240, H = 135, CX = 120, CY = 67;
static constexpr float FOCAL = 230.5f; // 120 px / tan(27,5°): 55° de campo horizontal

// --- Colores (RGB565) -------------------------------------------------------------------------
static constexpr uint32_t rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
static constexpr uint32_t C_BG = rgb(4, 6, 14);
static constexpr uint32_t C_GROUND = rgb(14, 12, 12);
static constexpr uint32_t C_DIM = rgb(100, 110, 130);
static constexpr uint32_t C_TEXT = rgb(230, 235, 245);
static constexpr uint32_t C_RED = rgb(255, 70, 60);
static constexpr uint32_t C_GREEN = rgb(70, 225, 120);
static constexpr uint32_t C_YELLOW = rgb(255, 205, 50);
static constexpr uint32_t C_ACCENT = rgb(80, 170, 255);
static constexpr uint32_t C_LINES = rgb(34, 42, 62);   // constelaciones
static constexpr uint32_t C_HORIZON = rgb(70, 110, 80);
static constexpr uint32_t C_SAT = rgb(90, 235, 235);
static constexpr uint32_t C_AIR = rgb(255, 160, 60);
static constexpr uint32_t C_NAME = rgb(120, 140, 175);

// --- Estado general -----------------------------------------------------------------------------
enum Layer : uint8_t { L_STARS, L_SATS, L_AIR, L_ALL };
static const char *LAYER_NAME[4] = {"Estrellas", "Satélites", "Aéreos", "Todo"};

static cpos_canvas_t *cv = nullptr, *scr = nullptr;
static Layer layer = L_STARS;
static uint8_t axisMap = 0; // 0..7: giro de 90° en Z y volteo del sensor
static int prefsH = -1;

static char toast[56] = "";
static uint32_t toastUntil = 0;

static void showToast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toastUntil = millis() + 2500;
}

static float wrap2pi(float a)
{
    while (a < 0) a += 2 * M_PI;
    while (a >= 2 * M_PI) a -= 2 * M_PI;
    return a;
}

static void txt(const char *s, int x, int y, uint32_t col, int datum = 0, int font = CPOS_FONT_JP12)
{
    GFX.set_font(cv, font);
    GFX.set_text_datum(cv, datum);
    GFX.set_text_color(cv, col, 0, 0);
    GFX.draw_string(cv, s, x, y);
}
enum { TL = 0, TC = 1, TR = 2, ML = 4, MC = 5, MR = 6, BL = 8, BC = 9, BR = 10 };

static void present()
{
    if (cv != scr) GFX.push(cv, scr, 0, 0, -1);
}

// --- Prefs (namespace skypointer) ----------------------------------------------------------------
static bool prefGet32(const char *key, int32_t &v)
{
    size_t n = 4;
    int32_t t;
    if (prefsH < 0 || cpos_api->prefs.get(prefsH, key, 0, &t, &n) != 0 || n != 4) return false;
    v = t;
    return true;
}
static void prefSet32(const char *key, int32_t v)
{
    if (prefsH >= 0) cpos_api->prefs.set(prefsH, key, CPOS_NVS_I32, &v, 4);
}

// --- Filtro de Mahony (el de la brújula) --------------------------------------------------------
// q lleva del cuerpo (X derecha, Y hacia el borde de arriba, Z saliendo de la pantalla) al mundo
// (Z hacia arriba). Solo el acelerómetro corrige, así que el error nunca tiene componente vertical:
// no toca el rumbo.
static float q0 = 1, q1 = 0, q2 = 0, q3 = 0;
static float kp = 1.0f;

static void mahony(float gx, float gy, float gz, float ax, float ay, float az, float dt)
{
    float n = sqrtf(ax * ax + ay * ay + az * az);
    if (n > 0.7f && n < 1.3f) { // con mucho sacudón el acelerómetro no mide la gravedad
        ax /= n, ay /= n, az /= n;
        float vx = 2 * (q1 * q3 - q0 * q2);
        float vy = 2 * (q0 * q1 + q2 * q3);
        float vz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;
        gx += kp * (ay * vz - az * vy);
        gy += kp * (az * vx - ax * vz);
        gz += kp * (ax * vy - ay * vx);
    }
    float a = q0, b = q1, c = q2;
    float h = 0.5f * dt;
    q0 += (-b * gx - c * gy - q3 * gz) * h;
    q1 += (a * gx + c * gz - q3 * gy) * h;
    q2 += (a * gy - b * gz + q3 * gx) * h;
    q3 += (a * gz + b * gy - c * gx) * h;
    n = 1.0f / sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    q0 *= n, q1 *= n, q2 *= n, q3 *= n;
}

struct Mat {
    float m[3][3];
};

static Mat R; // cuerpo -> mundo del filtro (el rumbo del mundo es arbitrario)

static Mat rotation()
{
    Mat r;
    r.m[0][0] = 1 - 2 * (q2 * q2 + q3 * q3);
    r.m[0][1] = 2 * (q1 * q2 - q0 * q3);
    r.m[0][2] = 2 * (q1 * q3 + q0 * q2);
    r.m[1][0] = 2 * (q1 * q2 + q0 * q3);
    r.m[1][1] = 1 - 2 * (q1 * q1 + q3 * q3);
    r.m[1][2] = 2 * (q2 * q3 - q0 * q1);
    r.m[2][0] = 2 * (q1 * q3 - q0 * q2);
    r.m[2][1] = 2 * (q2 * q3 + q0 * q1);
    r.m[2][2] = 1 - 2 * (q1 * q1 + q2 * q2);
    return r;
}

// --- Sensor ---------------------------------------------------------------------------------------
static float bias[3] = {0, 0, 0}; // sesgo del giróscopo (°/s)
static bool calibrating = true;
static int calN = 0;
static float calSum[3];
static constexpr int CAL_SAMPLES = 150;
static uint32_t stillSince = 0, lastUs = 0;

// Lleva un vector de los ejes del chip a los ejes del Cardputer.
static void mapAxes(float &x, float &y, float &z)
{
    for (int i = 0; i < (axisMap & 3); ++i) {
        float t = x;
        x = -y;
        y = t;
    }
    if (axisMap & 4) x = -x, z = -z;
}

static void startCalibration()
{
    calibrating = true;
    calN = 0;
    memset(calSum, 0, sizeof(calSum));
    kp = 8.0f; // converge rápido a la gravedad mientras está quieto
}

// Rumbo crudo (en el mundo del filtro): hacia dónde apunta el borde de arriba, o la parte de atrás
// cuando lo tenés parado. La suma de las dos proyecciones horizontales pasa suave de un caso al otro.
static float rawHeading(const Mat &r)
{
    return atan2f(r.m[0][1] - r.m[0][2], r.m[1][1] - r.m[1][2]);
}

static void readImu()
{
    if (!cpos_api->imu.update()) return;
    float ax, ay, az, gx, gy, gz;
    cpos_api->imu.accel(&ax, &ay, &az);
    cpos_api->imu.gyro(&gx, &gy, &gz);
    mapAxes(ax, ay, az);
    mapAxes(gx, gy, gz);

    uint32_t now = micros();
    float dt = lastUs ? (now - lastUs) * 1e-6f : 0.01f;
    lastUs = now;
    if (dt > 0.1f) dt = 0.1f;

    if (calibrating) {
        // Si se mueve, empieza de nuevo.
        if (calN > 0) {
            float g[3] = {gx, gy, gz}, mg = 0;
            for (int i = 0; i < 3; ++i) mg = fmaxf(mg, fabsf(g[i] - calSum[i] / calN));
            if (mg > 4.0f) calN = 0, memset(calSum, 0, sizeof(calSum));
        }
        calSum[0] += gx, calSum[1] += gy, calSum[2] += gz;
        ++calN;
        if (calN >= CAL_SAMPLES) {
            for (int i = 0; i < 3; ++i) bias[i] = calSum[i] / calN;
            calibrating = false;
            kp = 1.0f;
        }
    } else {
        // Quieto un rato: va afinando el sesgo de a poco.
        float m = fabsf(gx - bias[0]) + fabsf(gy - bias[1]) + fabsf(gz - bias[2]);
        float an = sqrtf(ax * ax + ay * ay + az * az);
        uint32_t ms = millis();
        if (m < 1.5f && fabsf(an - 1) < 0.03f) {
            if (!stillSince) stillSince = ms;
            if (ms - stillSince > 500) {
                bias[0] += 0.002f * (gx - bias[0]);
                bias[1] += 0.002f * (gy - bias[1]);
                bias[2] += 0.002f * (gz - bias[2]);
            }
        } else {
            stillSince = 0;
        }
    }

    float bx = calibrating ? 0 : bias[0], by = calibrating ? 0 : bias[1], bz = calibrating ? 0 : bias[2];
    if (calibrating) gx = gy = gz = 0; // mientras calibra solo cuenta la gravedad
    mahony((gx - bx) * RAD, (gy - by) * RAD, (gz - bz) * RAD, ax, ay, az, dt);
    R = rotation();
}

// --- Rumbo: del mundo del filtro al ENU --------------------------------------------------------------
// bearing_ENU = bearing_mundo - yawOff. Renu lleva del cuerpo a ENU (x = Este, y = Norte, z = Arriba).
static float yawOff = 0;
static bool yawSet = false;
static uint32_t yawFixAt = 0;
static float yawBase = 0;                     // error con el que quedó el último ajuste (°)
static constexpr float DRIFT_DPS = 0.02f;     // deriva estimada del giróscopo ya calibrado (°/s)
static Mat Renu;

static void updateRenu()
{
    float c = cosf(yawOff), s = sinf(yawOff);
    for (int j = 0; j < 3; ++j) {
        Renu.m[0][j] = c * R.m[0][j] - s * R.m[1][j];
        Renu.m[1][j] = s * R.m[0][j] + c * R.m[1][j];
        Renu.m[2][j] = R.m[2][j];
    }
}

static float yawError()
{
    return yawBase + DRIFT_DPS * (millis() - yawFixAt) * 0.001f;
}

// Vector ENU -> coordenadas de pantalla. d > 0: el punto está delante del equipo.
static bool project(float e, float n, float u, float &px, float &py, float &d)
{
    float xb = Renu.m[0][0] * e + Renu.m[1][0] * n + Renu.m[2][0] * u;
    float yb = Renu.m[0][1] * e + Renu.m[1][1] * n + Renu.m[2][1] * u;
    d = -(Renu.m[0][2] * e + Renu.m[1][2] * n + Renu.m[2][2] * u); // boresight = -Z
    if (d < 0.03f) return false;
    px = CX + FOCAL * xb / d;
    py = CY - FOCAL * yb / d;
    return true;
}

static void enuFromAzAlt(float az, float alt, float &e, float &n, float &u)
{
    float ca = cosf(alt);
    e = ca * sinf(az);
    n = ca * cosf(az);
    u = sinf(alt);
}

// --- Hora y posición ---------------------------------------------------------------------------------
static uint32_t lastU = 0, lastUms = 0;
static bool secTick = false;
static double obsLat = 0, obsLon = 0;
static bool havePos = false, posLive = false;
static int32_t savedLat = 0, savedLon = 0;
static uint32_t lastSave = 0;

static void tickTime()
{
    uint32_t u = nowUnix();
    if (u != lastU) {
        lastU = u;
        lastUms = millis();
        secTick = true;
    }
}

static double jdNow()
{
    uint32_t ms = millis() - lastUms;
    if (ms > 999) ms = 999;
    return (lastU + ms * 0.001) / 86400.0 + 2440587.5; // UTC; la diferencia con UT1 no se ve
}

static void updatePosition()
{
    posLive = G.fix;
    if (G.fix) {
        obsLat = G.lat * 1e-7;
        obsLon = G.lon * 1e-7;
        havePos = true;
        // guarda la última posición para cuando no haya fix (cada tanto y si se movió)
        if (!lastSave || millis() - lastSave > 60000) {
            if (labs((long)G.lat - savedLat) > 5000 || labs((long)G.lon - savedLon) > 5000) {
                savedLat = G.lat, savedLon = G.lon;
                prefSet32("lat", savedLat);
                prefSet32("lon", savedLon);
            }
            lastSave = millis() ? millis() : 1;
        }
    }
}

// --- Estrellas, nombres y colores -------------------------------------------------------------------
// El catálogo está ordenado de la más brillante a la más débil: estos son los índices de las que
// llevan nombre (identificadas por su ascensión recta y declinación).
static const char *const STAR_NAME[48] = {
    "Sirio", "Canopus", "Arturo", "Rigil Kent.", "Vega", "Capella", "Rigel", "Procyon", "Achernar", "Betelgeuse",
    "Hadar", "Altair", "Acrux", "Aldebarán", nullptr, "Spica", "Antares", "Pólux", "Fomalhaut", "Mimosa",
    "Deneb", nullptr, "Régulus", "Adhara", "Cástor", "Gacrux", "Shaula", "Bellatrix", "Elnath", "Miaplacidus",
    "Alnilam", "Alnair", "Alnitak", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "Polaris"};

// Un color por nivel de brillo. El catálogo ya no guarda el índice de color
// (cada byte del bloque de datos cuesta RAM al cargar), así que va un blanco tibio.
static uint16_t starCol[4];

static void buildPalette()
{
    static const float lvl[4] = {0.42f, 0.62f, 0.82f, 1.0f};
    for (int l = 0; l < 4; ++l)
        starCol[l] = rgb((int)(255 * lvl[l]), (int)(245 * lvl[l]), (int)(225 * lvl[l]));
}

// --- Cielo: marco de horizonte, Sol, referencia -----------------------------------------------------
static sky::HorizonFrame hf;
static bool frameOk = false;
static float sunE, sunN, sunU, sunAltDeg, sunAzDeg;

struct Ref {
    int kind = 0; // 0 ninguna, 1 Sol, 2 estrella
    int idx = -1;
    float e = 0, n = 0, u = 0;
    float az = 0, alt = 0; // grados
    char name[20] = "";
} ref;

static void updateSky(double jd)
{
    hf.update(jd, obsLat, obsLon);
    frameOk = true;

    double ra, dec, az, alt;
    float x, y, z;
    sky::sun(jd, ra, dec);
    sky::toUnit(ra, dec, x, y, z);
    hf.altAz(x, y, z, az, alt);
    sunAzDeg = (float)(az * DEGF);
    sunAltDeg = (float)(alt * DEGF);
    alt += sky::refraction(alt); // el Sol se ve más alto que su posición geométrica
    enuFromAzAlt((float)az, (float)alt, sunE, sunN, sunU);

    // referencia para fijar el rumbo: el Sol si está alto, si no la estrella más brillante sobre 20°
    ref.kind = 0;
    if (sunAltDeg > 10.0f) {
        ref.kind = 1;
        ref.e = sunE, ref.n = sunN, ref.u = sunU;
        ref.az = sunAzDeg, ref.alt = sunAltDeg;
        snprintf(ref.name, sizeof(ref.name), "el Sol");
    } else {
        for (int i = 0; i < SKY_STAR_COUNT; ++i) {
            const SkyStar &s = SKY_STARS[i];
            float e, n, u;
            hf.vec(s.x / 32767.0f, s.y / 32767.0f, s.z / 32767.0f, e, n, u);
            if (u > 0.342f) { // sen 20°
                ref.kind = 2, ref.idx = i;
                ref.e = e, ref.n = n, ref.u = u;
                ref.az = wrap2pi(atan2f(e, n)) * DEGF;
                ref.alt = asinf(fminf(1.0f, u)) * DEGF;
                if (i < 48 && STAR_NAME[i]) snprintf(ref.name, sizeof(ref.name), "%s", STAR_NAME[i]);
                else snprintf(ref.name, sizeof(ref.name), "mag %.1f", (s.mag10 - 20) * 0.1f);
                break;
            }
        }
    }
}

// --- Satélites ----------------------------------------------------------------------------------------
static constexpr int MAX_SATS = 2, TRK_N = 20; // traza de 10 minutos, un punto cada 30 s
// El ElsetRec del SGP4 pesa ~1,6 KB por satélite. Se piden al heap sólo cuando
// hay TLE: el OS tiene que reservar todo el segmento de datos en un solo bloque
// contiguo, y ese bloque es lo que no entraba.
static Sat *sats = nullptr;
static int nSats = 0;
struct SatPos {
    bool ok = false;
    float e = 0, n = 0, u = 0;
    float altDeg = 0;
    char label[12] = "";
    bool trkOk[TRK_N];
    float te[TRK_N], tn[TRK_N], tu[TRK_N];
} satPos[MAX_SATS];
static int trkCursor = 0;

static const char *TLE_FILE = "/sdcard/skypointer/tle.txt";
static const char *AIR_FILE = "/sdcard/skypointer/aircraft.json";

static void trimName(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    while (src[i] && src[i] != ' ' && src[i] != '(' && i + 1 < n) dst[i] = src[i], ++i;
    dst[i] = 0;
}

// Lee los TLE de un texto (nombre opcional + línea 1 + línea 2). Devuelve cuántos cargó.
static int parseTles(const char *text)
{
    if (!sats && !(sats = (Sat *)cpos_api->malloc(sizeof(Sat) * MAX_SATS))) return 0;
    memset(sats, 0, sizeof(Sat) * MAX_SATS);
    int n = 0;
    char l[100], prev[28] = "";
    const char *p = text;
    while (*p && n < MAX_SATS) {
        size_t k = 0;
        while (*p && *p != '\n') {
            if (*p != '\r' && k < sizeof(l) - 1) l[k++] = *p;
            ++p;
        }
        if (*p == '\n') ++p;
        l[k] = 0;
        if (l[0] == '1' && l[1] == ' ' && k >= 69) {
            char l2[100];
            size_t m = 0;
            while (*p && *p != '\n') {
                if (*p != '\r' && m < sizeof(l2) - 1) l2[m++] = *p;
                ++p;
            }
            if (*p == '\n') ++p;
            l2[m] = 0;
            if (l2[0] == '2' && m >= 69 && satFromTle(l, l2, sats[n], prev)) {
                SatPos &sp = satPos[n];
                sp.ok = false;
                trimName(sp.label, sizeof(sp.label), prev[0] ? prev : "SAT");
                memset(sp.trkOk, 0, sizeof(sp.trkOk));
                ++n;
            }
            prev[0] = 0;
        } else if (l[0] && l[0] != '2') {
            snprintf(prev, sizeof(prev), "%.27s", l);
        }
    }
    nSats = n;
    return n;
}

static bool loadTleFromSd()
{
    FILE *f = fopen(TLE_FILE, "rb");
    if (!f) return false;
    char buf[520];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return parseTles(buf) > 0;
}

static bool loadTleFromPrefs()
{
    if (prefsH < 0) return false;
    char buf[520];
    size_t n = sizeof(buf) - 1;
    if (cpos_api->prefs.get(prefsH, "tle", CPOS_NVS_BLOB, buf, &n) != 0 || n == 0) return false;
    buf[n < sizeof(buf) - 1 ? n : sizeof(buf) - 1] = 0;
    return parseTles(buf) > 0;
}

static void saveTle(const char *text)
{
    size_t n = strlen(text);
    if (n > 500) return;
    mkdir("/sdcard/skypointer", 0777);
    FILE *f = fopen(TLE_FILE, "wb");
    if (f) {
        fwrite(text, 1, n, f);
        fclose(f);
    }
    if (prefsH >= 0) cpos_api->prefs.set(prefsH, "tle", CPOS_NVS_BLOB, text, n + 1);
}

static double tleAgeDays(double jd)
{
    return nSats ? jd - sats[0].epochJd : 1e9;
}

static void satToEnu(const Sat &s, double jd, float &e, float &n, float &u, float &altDeg, bool &ok)
{
    double az, alt, range;
    ok = satAzAlt(s, jd, obsLat, obsLon, az, alt, range);
    if (!ok) return;
    altDeg = (float)(alt * DEGF);
    enuFromAzAlt((float)az, (float)alt, e, n, u);
}

// Posición actual de cada satélite (cada cuadro) y un punto de la traza por cuadro.
static void updateSats(double jd)
{
    for (int i = 0; i < nSats; ++i) {
        SatPos &sp = satPos[i];
        satToEnu(sats[i], jd, sp.e, sp.n, sp.u, sp.altDeg, sp.ok);
    }
    if (!nSats) return;
    int total = nSats * TRK_N;
    trkCursor = (trkCursor + 1) % total;
    int si = trkCursor / TRK_N, k = trkCursor % TRK_N;
    SatPos &sp = satPos[si];
    float alt;
    satToEnu(sats[si], jd + (k + 1) * 30.0 / 86400.0, sp.te[k], sp.tn[k], sp.tu[k], alt, sp.trkOk[k]);
}

// --- Tráfico aéreo (de la SD) --------------------------------------------------------------------------
struct Aircraft {
    char flight[9];
    float lat, lon;
    int32_t altFt;
    int16_t track; // -1 sin dato
    float e, n, u;      // dirección ENU
    float te, tn, tu;   // un punto 3 km adelante, para orientar el triángulo
    bool ok;            // sobre el horizonte
};
static constexpr int MAX_AIR = 32;
static Aircraft air[MAX_AIR];
static int nAir = 0;
static bool airLoaded = false;

// Azimut y altura (rad) de un avión desde el observador: ENU plano, la Tierra como esfera local de
// radio 6371 km. Alcanza a decenas de kilómetros. (jd no entra en la cuenta: el avión es un dato fijo.)
static void aircraftAzAlt(double lat, double lon, double altFt, double jd, double obsLat_, double obsLon_,
                          double &az, double &alt)
{
    (void)jd;
    const double RE = 6371000.0, D = M_PI / 180.0;
    double dN = (lat - obsLat_) * D * RE;
    double dE = (lon - obsLon_) * D * RE * cos((lat + obsLat_) * 0.5 * D);
    double hor = sqrt(dE * dE + dN * dN);
    double up = altFt * 0.3048 - hor * hor / (2 * RE); // altura sobre el plano local (con curvatura)
    az = atan2(dE, dN);
    if (az < 0) az += 2 * M_PI;
    alt = atan2(up, hor);
}

static void airToEnu(Aircraft &a, double jd)
{
    double az, alt;
    aircraftAzAlt(a.lat, a.lon, a.altFt, jd, obsLat, obsLon, az, alt);
    a.ok = alt > 0;
    enuFromAzAlt((float)az, (float)alt, a.e, a.n, a.u);
    a.te = a.tn = a.tu = 0;
    if (a.track >= 0) {
        double t = a.track * RAD, la = a.lat + 3000.0 * cos(t) / 6371000.0 / RAD;
        double lo = a.lon + 3000.0 * sin(t) / (6371000.0 * RAD * cos(a.lat * RAD));
        aircraftAzAlt(la, lo, a.altFt, jd, obsLat, obsLon, az, alt);
        enuFromAzAlt((float)az, (float)alt, a.te, a.tn, a.tu);
    }
}

// busca "clave": en el objeto y lee un número
static bool jsonNum(const char *o, const char *key, double &v)
{
    char pat[20];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(o, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') ++p;
    char *e;
    v = strtod(p, &e);
    return e != p;
}

static bool jsonStr(const char *o, const char *key, char *out, size_t n)
{
    char pat[20];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(o, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    while (i && out[i - 1] == ' ') --i;
    out[i] = 0;
    return i > 0;
}

static void addAircraft(const char *o)
{
    double lat, lon, alt, trk;
    if (!jsonNum(o, "lat", lat) || !jsonNum(o, "lon", lon)) return;
    if (!jsonNum(o, "alt", alt) && !jsonNum(o, "alt_baro", alt) && !jsonNum(o, "alt_geom", alt)) return; // en tierra: "ground"
    if (!jsonNum(o, "track", trk)) trk = -1;
    Aircraft a;
    memset(&a, 0, sizeof(a));
    if (!jsonStr(o, "flight", a.flight, sizeof(a.flight)) && !jsonStr(o, "r", a.flight, sizeof(a.flight)) &&
        !jsonStr(o, "hex", a.flight, sizeof(a.flight)))
        snprintf(a.flight, sizeof(a.flight), "?");
    a.lat = (float)lat, a.lon = (float)lon;
    a.altFt = (int32_t)alt;
    a.track = trk >= 0 ? (int16_t)(fmod(trk, 360.0)) : -1;
    int slot = nAir;
    if (nAir >= MAX_AIR) { // lleno: se queda con los más cercanos
        if (!havePos) return;
        auto d2 = [](float la, float lo) {
            double dn = la - obsLat, de = (lo - obsLon) * cos(obsLat * RAD);
            return dn * dn + de * de;
        };
        int far = 0;
        for (int i = 1; i < nAir; ++i)
            if (d2(air[i].lat, air[i].lon) > d2(air[far].lat, air[far].lon)) far = i;
        if (d2(a.lat, a.lon) >= d2(air[far].lat, air[far].lon)) return;
        slot = far;
    } else {
        ++nAir;
    }
    air[slot] = a;
}

// Lee /skypointer/aircraft.json: una lista de objetos, o el sobre {"ac":[...]} de adsb.lol / adsb.fi.
// Recorre el archivo de a pedazos y toma cada objeto del nivel de la lista, sin armar un árbol.
static int loadAircraft()
{
    nAir = 0;
    airLoaded = true;
    FILE *f = fopen(AIR_FILE, "rb");
    if (!f) return -1;
    char *chunk = (char *)cpos_api->malloc(512), *obj = (char *)cpos_api->malloc(1200);
    if (!chunk || !obj) {
        fclose(f);
        if (chunk) cpos_api->free(chunk);
        if (obj) cpos_api->free(obj);
        return -2;
    }
    int depth = 0, listDepth = -1;
    bool inStr = false, esc = false, capturing = false;
    size_t on = 0, n;
    while ((n = fread(chunk, 1, 512, f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            char c = chunk[i];
            if (capturing && on < 1199) obj[on++] = c;
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') {
                inStr = true;
            } else if (c == '[') {
                ++depth;
                if (listDepth < 0) listDepth = depth;
            } else if (c == '{') {
                if (listDepth > 0 && depth == listDepth && !capturing) {
                    capturing = true;
                    on = 0;
                    obj[on++] = '{';
                }
                ++depth;
            } else if (c == ']') {
                --depth;
            } else if (c == '}') {
                --depth;
                if (capturing && depth == listDepth) {
                    obj[on] = 0;
                    capturing = false;
                    addAircraft(obj);
                }
            }
        }
    }
    fclose(f);
    cpos_api->free(chunk);
    cpos_api->free(obj);
    return nAir;
}

static void updateAircraft(double jd)
{
    for (int i = 0; i < nAir; ++i) airToEnu(air[i], jd);
}

// --- Dibujo del cielo ----------------------------------------------------------------------------------
static float Mst[3][3]; // J2000 (int16 de catálogo) -> cuerpo, con la escala 1/32767
static float Hz[3][3];  // J2000 -> ENU

static void buildStarMatrix()
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0;
            for (int k = 0; k < 3; ++k) s += Renu.m[k][i] * hf.toHorizon.m[k][j];
            Mst[i][j] = s * (1.0f / 32767.0f);
            Hz[i][j] = hf.toHorizon.m[i][j];
        }
}

static inline bool bodyProject(const int16_t *v, float &px, float &py, float &zd)
{
    float x = v[0], y = v[1], z = v[2];
    float xb = Mst[0][0] * x + Mst[0][1] * y + Mst[0][2] * z;
    float yb = Mst[1][0] * x + Mst[1][1] * y + Mst[1][2] * z;
    zd = -(Mst[2][0] * x + Mst[2][1] * y + Mst[2][2] * z);
    if (zd < 0.03f) return false;
    px = CX + FOCAL * xb / zd;
    py = CY - FOCAL * yb / zd;
    return true;
}

static inline bool onScreen(float px, float py, float m = 0)
{
    return px >= -m && px < W + m && py >= -m && py < H + m;
}

static void drawConstellations()
{
    for (int i = 0; i < SKY_SEG_COUNT; ++i) {
        const SkyStar &sa = SKY_STARS[SKY_SEGS[i].a], &sb = SKY_STARS[SKY_SEGS[i].b];
        int16_t va[3] = {sa.x, sa.y, sa.z}, vb[3] = {sb.x, sb.y, sb.z};
        float ax, ay, ad, bx, by, bd;
        if (!bodyProject(va, ax, ay, ad) || !bodyProject(vb, bx, by, bd)) continue;
        if ((ax < 0 && bx < 0) || (ax >= W && bx >= W) || (ay < 0 && by < 0) || (ay >= H && by >= H)) continue;
        GFX.line(cv, (int)ax, (int)ay, (int)bx, (int)by, C_LINES);
    }
}

static void drawStars()
{
    for (int i = 0; i < SKY_STAR_COUNT; ++i) {
        const SkyStar &s = SKY_STARS[i];
        int16_t v[3] = {s.x, s.y, s.z};
        float px, py, zd;
        if (!bodyProject(v, px, py, zd) || !onScreen(px, py)) continue;
        int m10 = s.mag10 - 20;
        int lv = m10 < 20 ? 3 : m10 < 35 ? 2 : m10 < 50 ? 1 : 0;
        float up = Hz[2][0] * s.x + Hz[2][1] * s.y + Hz[2][2] * s.z; // bajo el horizonte se apagan
        if (up < 0) lv = 0;
        uint32_t col = starCol[lv];
        int x = (int)px, y = (int)py;
        if (m10 < 10) GFX.circle(cv, x, y, 1, col, 1);
        else if (m10 < 30) GFX.rect(cv, x, y, 2, 2, col, 1);
        else GFX.pixel(cv, x, y, col);
        if (i < 48 && STAR_NAME[i]) txt(STAR_NAME[i], x + 5, y - 6, C_NAME);
    }
}

static void drawSun()
{
    float px, py, d;
    if (!project(sunE, sunN, sunU, px, py, d) || !onScreen(px, py, 12)) return;
    int x = (int)px, y = (int)py;
    GFX.circle(cv, x, y, 9, rgb(70, 55, 10), 0);
    GFX.circle(cv, x, y, 7, rgb(120, 95, 15), 0);
    GFX.circle(cv, x, y, 4, rgb(255, 225, 90), 1);
    txt("Sol", x + 11, y - 6, C_YELLOW);
}

static void drawHorizon()
{
    // línea del horizonte (0°), de 5 en 5° de azimut; marcas cada 30°
    float ppx = 0, ppy = 0;
    bool prev = false;
    for (int a = 0; a <= 360; a += 5) {
        float e, n, u, px, py, d;
        enuFromAzAlt(a * RAD, 0, e, n, u);
        bool ok = project(e, n, u, px, py, d);
        if (ok && prev) GFX.line(cv, (int)ppx, (int)ppy, (int)px, (int)py, C_HORIZON);
        prev = ok, ppx = px, ppy = py;
        if (ok && a % 30 == 0 && a < 360 && px > 6 && px < W - 6 && py > 16 && py < H - 16) {
            GFX.line(cv, (int)px, (int)py - 4, (int)px, (int)py + 4, C_HORIZON);
            static const char *card[12] = {"N", "30", "60", "E", "120", "150", "S", "210", "240", "O", "300", "330"};
            const char *s = card[a / 30];
            txt(s, (int)px, (int)py - 6, a % 90 == 0 ? (a == 0 ? C_RED : C_TEXT) : C_DIM, BC);
        }
    }
}

static void drawRefMarker()
{
    if (!ref.kind) return;
    float px, py, d;
    if (project(ref.e, ref.n, ref.u, px, py, d) && onScreen(px, py, 4)) {
        GFX.circle(cv, (int)px, (int)py, 11, C_GREEN, 0);
        return;
    }
    // fuera de pantalla: una flecha en el borde hacia donde está
    float xb = Renu.m[0][0] * ref.e + Renu.m[1][0] * ref.n + Renu.m[2][0] * ref.u;
    float yb = Renu.m[0][1] * ref.e + Renu.m[1][1] * ref.n + Renu.m[2][1] * ref.u;
    float l = sqrtf(xb * xb + yb * yb);
    if (l < 1e-4f) return;
    float dx = xb / l, dy = -yb / l;
    int ax = CX + (int)(dx * 106), ay = CY + (int)(dy * 56);
    float nx = -dy, ny = dx;
    GFX.triangle(cv, ax + (int)(dx * 6), ay + (int)(dy * 6), ax + (int)(nx * 4), ay + (int)(ny * 4),
                 ax - (int)(nx * 4), ay - (int)(ny * 4), C_GREEN, 1);
}

static void drawSats()
{
    for (int i = 0; i < nSats; ++i) {
        const SatPos &sp = satPos[i];
        if (!sp.ok) continue;
        for (int k = 0; k < TRK_N; ++k) { // traza punteada de los próximos 10 minutos
            if (!sp.trkOk[k] || sp.tu[k] < 0) continue;
            float px, py, d;
            if (project(sp.te[k], sp.tn[k], sp.tu[k], px, py, d) && onScreen(px, py)) GFX.pixel(cv, (int)px, (int)py, rgb(40, 130, 130));
        }
        float px, py, d;
        if (!project(sp.e, sp.n, sp.u, px, py, d) || !onScreen(px, py, 8)) continue;
        uint32_t col = sp.altDeg > 0 ? C_SAT : rgb(50, 80, 90); // bajo el horizonte no se ve
        int x = (int)px, y = (int)py;
        GFX.hline(cv, x - 5, y, 11, col);
        GFX.vline(cv, x, y - 5, 11, col);
        txt(sp.label, x + 7, y - 6, col);
    }
}

static void drawAircraft()
{
    for (int i = 0; i < nAir; ++i) {
        const Aircraft &a = air[i];
        float px, py, d;
        if (!a.ok || !project(a.e, a.n, a.u, px, py, d) || !onScreen(px, py, 8)) continue;
        // orientación en pantalla: hacia el punto 3 km adelante sobre su rumbo
        float dx = 0, dy = -1, qx, qy, qd;
        if (a.track >= 0 && project(a.te, a.tn, a.tu, qx, qy, qd)) {
            float lx = qx - px, ly = qy - py, l = sqrtf(lx * lx + ly * ly);
            if (l > 0.01f) dx = lx / l, dy = ly / l;
        }
        float nx = -dy, ny = dx;
        int x = (int)px, y = (int)py;
        GFX.triangle(cv, x + (int)(dx * 6), y + (int)(dy * 6), x - (int)(dx * 4 - nx * 3.5f), y - (int)(dy * 4 - ny * 3.5f),
                     x - (int)(dx * 4 + nx * 3.5f), y - (int)(dy * 4 + ny * 3.5f), C_AIR, 1);
        char lb[24];
        snprintf(lb, sizeof(lb), "%.8s %ld", a.flight, (long)a.altFt);
        txt(lb, x + 8, y - 6, C_AIR);
    }
}

// --- Pantallas -------------------------------------------------------------------------------------------
static void clock3(char *buf, size_t n, uint32_t u)
{
    uint32_t s = u % 86400;
    snprintf(buf, n, "%02u:%02u:%02uZ", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

static void renderWaiting(bool timeOk)
{
    GFX.fill(cv, C_BG);
    txt("Buscador de cielo", 120, 22, C_TEXT, MC, CPOS_FONT_JP16);
    txt("Esperando hora y posición", 120, 44, C_YELLOW, MC);
    char b[48];
    if (!G.up) snprintf(b, sizeof(b), "GPS: sin módulo (Cap LoRa-1262)");
    else if (G.fix) snprintf(b, sizeof(b), "GPS: posición lista · %u sat", G.sats);
    else snprintf(b, sizeof(b), "GPS: buscando · %u sat", G.sats);
    txt(b, 120, 66, G.fix ? C_GREEN : C_DIM, MC);
    snprintf(b, sizeof(b), "Hora: %s", timeOk ? "lista" : "sin hora");
    txt(b, 120, 82, timeOk ? C_GREEN : C_DIM, MC);
    snprintf(b, sizeof(b), "Posición: %s", havePos ? (posLive ? "GPS" : "guardada") : "ninguna");
    txt(b, 120, 98, havePos ? C_GREEN : C_DIM, MC);
    txt("s: poner la hora por WiFi (NTP)", 120, 120, C_ACCENT, MC);
}

static void renderCalibration()
{
    GFX.fill(cv, C_BG);
    txt("Calibrando el giróscopo", 120, 46, C_TEXT, MC, CPOS_FONT_JP16);
    txt("Dejalo quieto sobre la mesa", 120, 68, C_DIM, MC);
    GFX.round_rect(cv, 40, 84, 160, 8, 4, rgb(26, 32, 46), 1);
    int w = 160 * calN / CAL_SAMPLES;
    GFX.round_rect(cv, 40, 84, w < 8 ? 8 : w, 8, 4, C_ACCENT, 1);
    if (!cpos_api->imu.enabled()) txt("No se encontró el sensor (IMU)", 120, 110, C_RED, MC);
}

static void renderHud()
{
    // dirección del boresight (-Z del cuerpo)
    float be = -Renu.m[0][2], bn = -Renu.m[1][2], bu = -Renu.m[2][2];
    float az = wrap2pi(atan2f(be, bn)) * DEGF, alt = asinf(fmaxf(-1.0f, fminf(1.0f, bu))) * DEGF;
    char b[48];
    snprintf(b, sizeof(b), "AZ %03d°  ALT %+d°", (int)lroundf(az) % 360, (int)lroundf(alt));
    txt(b, 4, 2, C_TEXT);
    clock3(b, sizeof(b), lastU);
    txt(b, 236, 2, C_TEXT, TR);
    snprintf(b, sizeof(b), "%s · GPS %u", LAYER_NAME[layer], G.sats);
    txt(b, 4, 15, posLive ? C_ACCENT : C_YELLOW);
    if (!posLive) txt("pos guardada", 236, 15, C_YELLOW, TR);

    // cruz central
    GFX.hline(cv, CX - 8, CY, 5, C_TEXT);
    GFX.hline(cv, CX + 4, CY, 5, C_TEXT);
    GFX.vline(cv, CX, CY - 8, 5, C_TEXT);
    GFX.vline(cv, CX, CY + 4, 5, C_TEXT);

    // pie: aviso, o la referencia que propone la app
    if (millis() < toastUntil) {
        txt(toast, 4, 134, C_ACCENT, BL);
    } else if (!yawSet) {
        txt("Fijá el rumbo: n (norte) o enter", 4, 134, C_YELLOW, BL);
    } else if (ref.kind) {
        snprintf(b, sizeof(b), "enter: %s (az %d° alt %d°)", ref.name, (int)lroundf(ref.az), (int)lroundf(ref.alt));
        txt(b, 4, 134, C_DIM, BL);
    }
    if (yawSet) {
        float e = yawError();
        snprintf(b, sizeof(b), "rumbo ±%d°", (int)ceilf(e));
        txt(b, 236, 134, e < 2.0f ? C_GREEN : e < 5.0f ? C_YELLOW : C_RED, BR);
    } else if (ref.kind) {
        snprintf(b, sizeof(b), "ref: %s", ref.name);
        txt(b, 236, 134, C_DIM, BR);
    }
}

static void render()
{
    if (calibrating) {
        renderCalibration();
        present();
        return;
    }
    bool timeOk = lastU != 0;
    if (!timeOk || !havePos || !frameOk) {
        renderWaiting(timeOk);
        present();
        return;
    }
    updateRenu();
    GFX.fill(cv, C_BG);
    // el suelo, apenas más oscuro: una franja bajo el horizonte del centro de la pantalla
    double jd = jdNow();
    buildStarMatrix();
    bool stars = layer == L_STARS || layer == L_ALL;
    if (stars) drawConstellations();
    drawHorizon();
    if (stars) drawStars();
    drawSun();
    if (layer == L_SATS || layer == L_ALL) {
        updateSats(jd);
        drawSats();
    }
    if (layer == L_AIR || layer == L_ALL) drawAircraft();
    drawRefMarker();
    renderHud();
    present();
}

// --- Sincronización por WiFi (reloj y TLE) -------------------------------------------------------------
struct SockAddr {
    uint8_t len, family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};
static inline uint16_t htons16(uint16_t p) { return (uint16_t)((p & 0xFF) << 8 | p >> 8); }

static void syncMsg(const char *a, const char *b = "")
{
    GFX.fill(cv, C_BG);
    txt("Sincronizando", 120, 34, C_TEXT, MC, CPOS_FONT_JP16);
    txt(a, 120, 64, C_ACCENT, MC);
    txt(b, 120, 82, C_DIM, MC);
    txt("Dejalo quieto: el rumbo se pierde mientras tanto", 120, 122, C_DIM, MC);
    present();
}

static bool waitMs(uint32_t ms)
{
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        if (cpos_api->exit_requested()) return false;
        pollGnss();
        readImu();
        cpos_api->delay_ms(10);
    }
    return true;
}

static bool ntpSync(const char **err)
{
    uint32_t ip = 0;
    if (NET.resolve("pool.ntp.org", &ip)) { *err = "NTP: sin DNS"; return false; }
    int fd = NET.socket(2, 2, 0);
    if (fd < 0) { *err = "NTP: sin socket"; return false; }
    NET.fcntl(fd, 4 /*F_SETFL*/, 1 /*O_NONBLOCK*/);
    SockAddr a = {sizeof(a), 2, htons16(123), ip, {0}};
    uint8_t pkt[48], rsp[48];
    bool ok = false;
    for (int tries = 0; tries < 3 && !ok; ++tries) {
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = (0 << 6) | (4 << 3) | 3; // LI=0 VN=4 Mode=3 (cliente)
        NET.sendto(fd, pkt, sizeof(pkt), 0, &a, sizeof(a));
        for (uint32_t t0 = millis(); millis() - t0 < 1500 && !ok;) {
            SockAddr from;
            uint32_t fl = sizeof(from);
            int r = NET.recvfrom(fd, rsp, sizeof(rsp), 0, &from, &fl);
            if (r >= 48 && (rsp[0] & 7) == 4 && rsp[1] != 0) {
                uint32_t secs = (uint32_t)rsp[40] << 24 | (uint32_t)rsp[41] << 16 | (uint32_t)rsp[42] << 8 | rsp[43];
                if (secs > 3913056000u) { // después de 2024
                    gnss_set_time(secs - 2208988800u);
                    ok = true;
                }
            } else if (!waitMs(40)) break;
        }
    }
    NET.close(fd);
    if (!ok) *err = "NTP: sin respuesta";
    return ok;
}

// GET http://host/path por HTTP plano (no hay TLS). Copia el cuerpo en out. Devuelve su largo o -1.
static int httpGet(const char *host, const char *path, char *out, size_t cap, const char **err)
{
    uint32_t ip = 0;
    if (NET.resolve(host, &ip)) { *err = "TLE: sin DNS"; return -1; }
    int fd = NET.socket(2, 1, 0);
    if (fd < 0) { *err = "TLE: sin socket"; return -1; }
    SockAddr a = {sizeof(a), 2, htons16(80), ip, {0}};
    if (NET.sock_connect(fd, &a, sizeof(a)) != 0) { NET.close(fd); *err = "TLE: sin conexión"; return -1; }
    NET.fcntl(fd, 4, 1);
    char req[200];
    int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: cardputeros-skypointer\r\nConnection: close\r\n\r\n", path, host);
    int sent = 0;
    for (uint32_t t0 = millis(); sent < rl && millis() - t0 < 5000;) {
        int r = NET.sock_send(fd, req + sent, rl - sent, 0);
        if (r > 0) sent += r;
        else if (!waitMs(30)) break;
    }
    char *raw = (char *)cpos_api->malloc(1400);
    if (!raw || sent < rl) {
        if (raw) cpos_api->free(raw);
        NET.close(fd);
        *err = "TLE: no se pudo pedir";
        return -1;
    }
    size_t got = 0;
    for (uint32_t t0 = millis(); got < 1399 && millis() - t0 < 10000;) {
        int r = NET.sock_recv(fd, raw + got, 1399 - got, 0);
        if (r > 0) got += r;
        else if (r == 0) break; // el servidor cerró: terminó
        else if (!waitMs(30)) break;
    }
    NET.close(fd);
    raw[got] = 0;
    int n = -1;
    const char *body = strstr(raw, "\r\n\r\n");
    if (got < 12 || strncmp(raw, "HTTP/1.", 7) != 0 || !strstr(raw, " 200")) *err = "TLE: Celestrak no contestó 200";
    else if (!body) *err = "TLE: respuesta cortada";
    else {
        body += 4;
        n = (int)strlen(body);
        if ((size_t)n >= cap) n = (int)cap - 1;
        memcpy(out, body, n);
        out[n] = 0;
    }
    cpos_api->free(raw);
    return n;
}

static void doSync()
{
    if (cpos_api->abi_minor < 7) { showToast("Hace falta CardputerOS 0.8 o posterior"); return; }
    bool wasUp = NET.status() == 3;
    syncMsg("Conectando a la red guardada", "System > WiFi del OS");
    if (!wasUp) {
        if (NET.connect("", nullptr) != 0) { showToast("No hay RAM para el WiFi"); return; }
        uint32_t t0 = millis();
        while (NET.status() != 3 && millis() - t0 < 20000) {
            if (!waitMs(200)) return;
        }
        if (NET.status() != 3) {
            NET.disconnect();
            NET.mode(0);
            showToast("Sin WiFi: guardá una red en System > WiFi");
            return;
        }
    }
    char msg[56] = "";
    const char *err = "";
    syncMsg("Pidiendo la hora", "pool.ntp.org");
    bool clockOk = ntpSync(&err);
    snprintf(msg, sizeof(msg), "%s", clockOk ? "Reloj OK" : err);
    if (clockOk && lastU == 0) tickTime();

    double jd = nowUnix() ? nowUnix() / 86400.0 + 2440587.5 : 0;
    if (nowUnix() && (nSats == 0 || tleAgeDays(jd) > 7.0)) {
        syncMsg("Bajando el TLE de la ISS", "celestrak.org (HTTP)");
        char *body = (char *)cpos_api->malloc(520);
        if (body) {
            int n = httpGet("celestrak.org", "/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE", body, 520, &err);
            if (n > 0 && parseTles(body) > 0) {
                saveTle(body);
                snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), " · TLE OK");
            } else {
                snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), " · %s", n > 0 ? "TLE inválido" : err);
                if (nSats == 0 && !loadTleFromSd()) loadTleFromPrefs(); // un parseo fallido no deja la lista a medias
            }
            cpos_api->free(body);
        }
    } else if (nSats) {
        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), " · TLE al día");
    }
    if (!wasUp) {
        NET.disconnect();
        NET.mode(0); // devuelve la RAM del driver de WiFi
    }
    showToast(msg);
    secTick = true;
}

// --- Teclado --------------------------------------------------------------------------------------------
static cpos_kbd_t kb, kbPrev;
static char repKey = 0;
static uint32_t repAt = 0;

static bool hasKey(const cpos_kbd_t &k, char c)
{
    for (int i = 0; i < k.count && i < CPOS_KBD_MAX_KEYS; ++i)
        if (k.keys[i] == c || k.keys[i] == (char)toupper(c)) return true;
    return false;
}

static void nudgeYaw(float deg)
{
    yawOff = wrap2pi(yawOff + deg * RAD); // una corrección a mano no cambia el error estimado
}

static void fixNorth()
{
    if (calibrating) return;
    yawOff = rawHeading(R); // el borde de arriba (o la parte de atrás) queda al norte
    yawSet = true;
    yawFixAt = millis();
    yawBase = 3.0f; // a ojo: ±3°
    showToast("Norte fijado");
}

static void fixReference()
{
    if (calibrating || !ref.kind || !frameOk) return;
    // dirección del boresight en el mundo del filtro
    float bx = -R.m[0][2], by = -R.m[1][2], bz = -R.m[2][2];
    if (bx * bx + by * by < 0.04f) { showToast("Apuntá más lejos del cenit"); return; }
    float bw = atan2f(bx, by);
    yawOff = wrap2pi(bw - ref.az * RAD);
    yawSet = true;
    yawFixAt = millis();
    float altErr = fabsf(asinf(fmaxf(-1.0f, fminf(1.0f, bz))) * DEGF - ref.alt); // qué tan bien apuntaste
    yawBase = fmaxf(0.5f, fminf(altErr, 10.0f));
    char b[56];
    snprintf(b, sizeof(b), "Rumbo fijado en %s (altura ±%.1f°)", ref.name, altErr);
    showToast(b);
}

static void setLayer(Layer l)
{
    layer = l;
    if (l == L_AIR || l == L_ALL) {
        int n = loadAircraft();
        if (n >= 0) updateAircraft(jdNow());
        char b[48];
        if (n < 0) snprintf(b, sizeof(b), "Sin /skypointer/aircraft.json en la SD");
        else snprintf(b, sizeof(b), "%d aviones en la SD", n);
        showToast(b);
    }
}

static void onKey(char c)
{
    switch (c) {
    case 'n': fixNorth(); break;
    case ',': case '<': nudgeYaw(-1); break;
    case '.': case '>': nudgeYaw(+1); break;
    case 's': doSync(); break;
    case 'c': startCalibration(); break;
    case 'x': {
        axisMap = (axisMap + 1) % 8;
        if (prefsH >= 0) {
            uint8_t v = axisMap;
            cpos_api->prefs.set(prefsH, "axis", CPOS_NVS_U8, &v, 1);
        }
        char b[40];
        snprintf(b, sizeof(b), "Orientación del sensor %d/8", axisMap + 1);
        showToast(b);
        startCalibration();
        break;
    }
    }
}

static void handleKeys()
{
    kbPrev = kb;
    cpos_api->kbd_state(&kb);
    if (kb.tab && !kbPrev.tab) setLayer((Layer)((layer + 1) % 4));
    if (kb.enter && !kbPrev.enter) fixReference();
    uint32_t now = millis();
    bool repHeld = false;
    for (int i = 0; i < kb.count && i < CPOS_KBD_MAX_KEYS; ++i) {
        char c = (char)tolower(kb.keys[i]);
        if (!hasKey(kbPrev, c)) {
            onKey(c);
            if (c == ',' || c == '.' || c == '<' || c == '>') repKey = c, repAt = now + 400;
        } else if (c == repKey) {
            repHeld = true;
            if (now >= repAt) onKey(c), repAt = now + 90;
        }
    }
    if (!repHeld && repKey && !hasKey(kb, repKey)) repKey = 0;
}

// --- Arduino --------------------------------------------------------------------------------------------
void setup()
{
    prefsH = cpos_api->prefs.open("skypointer", 0);
    uint8_t ax = 0;
    size_t n = 1;
    if (prefsH >= 0 && cpos_api->prefs.get(prefsH, "axis", 0, &ax, &n) == 0 && n == 1) axisMap = ax % 8;
    if (prefGet32("lat", savedLat) && prefGet32("lon", savedLon) && (savedLat || savedLon)) {
        obsLat = savedLat * 1e-7;
        obsLon = savedLon * 1e-7;
        havePos = true;
    }

    scr = GFX.screen();
    cv = GFX.create(W, H, 16);
    if (!cv) cv = scr; // sin RAM para el lienzo: dibuja directo (con parpadeo)
    buildPalette();
    R = rotation();
    updateRenu();
    startCalibration();
    G.up = cpos_api->lora.gnss_begin(115200) == 0; // sin el Cap LoRa-1262 sigue sin GPS (hora por NTP)
    if (!loadTleFromSd()) loadTleFromPrefs();
}

void loop()
{
    handleKeys();
    readImu();
    pollGnss();
    updatePosition();
    tickTime();
    if (secTick && lastU && havePos) {
        secTick = false;
        double jd = jdNow();
        updateSky(jd);
        if (layer == L_AIR || layer == L_ALL) updateAircraft(jd);
    }

    static uint32_t lastDraw = 0;
    if (millis() - lastDraw >= 33) {
        lastDraw = millis();
        render();
    }
    cpos_api->delay_ms(1);
}
