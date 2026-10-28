// Núcleo de la telemetría de karting: sin dependencias de Arduino, así se prueba en la PC
// con un simulador (apps/karting/sim).
//
// Cadena de estimación (todo con IMU, sin GPS, con el aparato en un bolsillo):
//   1. Calibración quieto: gravedad (dónde es "abajo") y sesgo del giróscopo.
//   2. Actitud (Mahony): rota la aceleración al marco de la Tierra. La corrección con la
//      gravedad se usa solo cuando la aceleración horizontal prevista es chica, para que
//      las curvas no inclinen la estimación.
//   3. Filtro plano: rumbo de la marcha por el giro vertical, velocidad por integración de la
//      aceleración tangencial, corregida con a_normal = v·ω en curva y con velocidad cero al
//      estar quieto. Dibuja posición (x, y) y distancia recorrida s.
//   4. Firma del circuito: el rumbo en función de la distancia (cada DS metros). Las vueltas
//      se detectan por repetición de esa firma, sin línea de meta.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace kart {

static constexpr float G = 9.80665f;
static constexpr float PI_F = 3.14159265f;

struct V3 {
    float x, y, z;
    V3() : x(0), y(0), z(0) {}
    V3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

inline float norm(const V3 &v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

struct Params {
    // Detección de arranque
    float startAccG = 0.08f;         // aceleración horizontal que cuenta como "se mueve"
    float startSpeed = 2.5f;         // m/s acumulados para declarar el arranque
    // Filtro plano
    float cornerMinW = 0.25f;        // rad/s para usar a_n = v·ω
    float cornerSigma = 1.5f;        // m/s² de ruido de la aceleración normal
    float speedQ = 0.6f;             // (m/s)²/s de ruido de proceso
    float biasGain = 0.05f;      // (m/s²)/s por cada m/s de diferencia en curva
    float zuptMaxSpeed = 4.0f;       // solo se asume quieto por debajo de esta velocidad
    float stillGyroDps = 1.5f;
    float stillAccG = 0.05f;
    float vibFactor = 1.5f;          // vibración < factor × la de reposo => parado
    float vibStillSec = 2.0f;
    // Firma
    float accLpHz = 6.0f;
    float attLpHz = 0.8f;            // filtro de la medición que corrige la actitud
    float attGate = 0.05f;           // g de aceleración horizontal tolerada para corregir
    float attKp = 0.2f;
    // Detección de vuelta
    float matchRms = 0.30f;          // rad, error cuadrático medio para dar por repetida una ventana
    float matchContent = 1.5f;       // rad de giro total mínimo en la ventana (evita rectas)
    int lockCount = 40;              // evaluaciones seguidas (c/2 muestras) que debe sostener la repetición
};

static constexpr float DS = 2.0f;           // m entre muestras de la firma
static constexpr int WIN = 40;              // muestras por ventana
static constexpr int LMIN = 100;            // vuelta mínima: 200 m
static constexpr int LMAX = 900;            // vuelta máxima: 1800 m
static constexpr int MAX_SAMPLES = 6000;    // 12 km

// --- Actitud -----------------------------------------------------------------------------
struct Attitude {
    float q0 = 1, q1 = 0, q2 = 0, q3 = 0;

    void initFromUp(const V3 &up)  // up: aceleración en reposo, normalizada (apunta hacia arriba)
    {
        q0 = 1 + up.z;
        q1 = up.y;
        q2 = -up.x;
        q3 = 0;
        if (q0 < 1e-4f) q0 = 0, q1 = 1, q2 = 0;
        normalize();
    }

    void normalize()
    {
        float n = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
        q0 /= n, q1 /= n, q2 /= n, q3 /= n;
    }

    // Dirección de "arriba" en el marco del cuerpo (tercera fila de la matriz cuerpo→Tierra).
    V3 up() const
    {
        return V3(2 * (q1 * q3 - q0 * q2), 2 * (q2 * q3 + q0 * q1), q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3);
    }

    V3 toEarth(const V3 &b) const
    {
        float r00 = q0 * q0 + q1 * q1 - q2 * q2 - q3 * q3, r01 = 2 * (q1 * q2 - q0 * q3), r02 = 2 * (q1 * q3 + q0 * q2);
        float r10 = 2 * (q1 * q2 + q0 * q3), r11 = q0 * q0 - q1 * q1 + q2 * q2 - q3 * q3, r12 = 2 * (q2 * q3 - q0 * q1);
        V3 u = up();
        return V3(r00 * b.x + r01 * b.y + r02 * b.z, r10 * b.x + r11 * b.y + r12 * b.z, u.x * b.x + u.y * b.y + u.z * b.z);
    }

    // w en rad/s (cuerpo). corr: corrección proporcional de la gravedad (rad/s por rad).
    void update(V3 w, const V3 *accNorm, float kp, float dt)
    {
        if (accNorm && kp > 0) {
            V3 v = up();
            w.x += kp * (accNorm->y * v.z - accNorm->z * v.y);
            w.y += kp * (accNorm->z * v.x - accNorm->x * v.z);
            w.z += kp * (accNorm->x * v.y - accNorm->y * v.x);
        }
        float hx = 0.5f * w.x * dt, hy = 0.5f * w.y * dt, hz = 0.5f * w.z * dt;
        float a = q0, b = q1, c = q2, d = q3;
        q0 += -b * hx - c * hy - d * hz;
        q1 += a * hx + c * hz - d * hy;
        q2 += a * hy - b * hz + d * hx;
        q3 += a * hz + b * hy - c * hx;
        normalize();
    }
};

// --- Firma del circuito y vueltas ------------------------------------------------------------
struct LapEvent {
    int number;          // 0, 1, 2... cruce de meta (0 = primer cruce, retroactivo)
    uint32_t atMs;       // instante del cruce
    uint32_t lapMs;      // duración de la vuelta que termina en este cruce (0 en el cruce 0)
    int laps;            // cuántas vueltas cubre (2 si se perdió un cruce)
    float idx;           // posición del cruce en las muestras de la firma
};

class Track {
public:
    std::vector<int16_t> th;    // rumbo desenrollado, 0,01 rad
    std::vector<uint32_t> tm;   // ms
    std::vector<int16_t> px, py;  // posición, 0,1 m
    std::vector<uint8_t> sp;    // velocidad, 0,25 m/s

    void reserve(int n)
    {
        th.reserve(n), tm.reserve(n), px.reserve(n), py.reserve(n), sp.reserve(n);
    }
    void clear()
    {
        th.clear(), tm.clear(), px.clear(), py.clear(), sp.clear();
    }
    int size() const { return (int)th.size(); }
    bool full() const { return size() >= MAX_SAMPLES; }

    void push(float theta, uint32_t t, float x, float y, float v)
    {
        th.push_back((int16_t)lroundf(constrain16(theta * 100.f)));
        tm.push_back(t);
        px.push_back((int16_t)lroundf(constrain16(x * 10.f)));
        py.push_back((int16_t)lroundf(constrain16(y * 10.f)));
        sp.push_back((uint8_t)std::min(255.f, std::max(0.f, v * 4.f)));
    }

    float thAt(float i) const
    {
        int k = (int)floorf(i);
        if (k < 0) return th[0] * 0.01f;
        if (k >= size() - 1) return th[size() - 1] * 0.01f;
        float f = i - k;
        return (th[k] * (1 - f) + th[k + 1] * f) * 0.01f;
    }

    float timeAt(float i) const
    {
        int k = (int)floorf(i);
        if (k < 0) return (float)tm[0];
        if (k >= size() - 1) return (float)tm[size() - 1];
        float f = i - k;
        return tm[k] * (1 - f) + tm[k + 1] * f;
    }

private:
    static float constrain16(float v) { return std::min(32767.f, std::max(-32767.f, v)); }
};

class LapDetector {
public:
    explicit LapDetector(const Params &p, const Track &t) : P(p), trk(t) {}

    void reset()
    {
        crossings.clear();
        phaseB = false;
        searchSkip = 0;
        candLag = 0;
        candCount = 0;
        L = 0;
        resetSearch();
    }

    // Se llama después de cada muestra nueva. Devuelve los cruces detectados (puede ser más de uno
    // la primera vez, porque se confirman de forma retroactiva).
    void update(std::vector<LapEvent> &out)
    {
        int n = trk.size();
        if (n < LMIN + WIN + 8) return;
        if (!phaseB) {
            if (n & 1) return;  // cada 2 muestras alcanza
            acquire(n, out);
        } else {
            track(n, out);
        }
    }

    bool locked() const { return phaseB; }
    float lapLength() const { return L; }  // en muestras

    // Trayectoria de una vuelta (entre los cruces i y i+1), cerrada: se reparte el desvío entre
    // el inicio y el final para que el circuito dibuje un lazo.
    void closedLap(int i, std::vector<float> &xs, std::vector<float> &ys, std::vector<uint8_t> &vs) const
    {
        xs.clear(), ys.clear(), vs.clear();
        if (i < 0 || i + 1 >= (int)crossings.size()) return;
        int a = (int)ceilf(crossings[i].idx), b = (int)floorf(crossings[i + 1].idx);
        if (b - a < 2) return;
        float dx = (trk.px[b] - trk.px[a]) * 0.1f, dy = (trk.py[b] - trk.py[a]) * 0.1f;
        for (int k = a; k <= b; k++) {
            float f = (float)(k - a) / (b - a);
            xs.push_back(trk.px[k] * 0.1f - dx * f);
            ys.push_back(trk.py[k] * 0.1f - dy * f);
            vs.push_back(trk.sp[k]);
        }
    }

    std::vector<LapEvent> crossings;

private:
    const Params &P;
    const Track &trk;
    bool phaseB = false;
    int candLag = 0, candCount = 0;
    float L = 0;           // largo de la vuelta, en muestras
    float scaleHint = 1.f;
    // búsqueda de la fase B
    float bestR = 1e9f;
    int bestB = -1;
    float bestScale = 1.f;
    int tplA = 0;
    float tplOff = 0;
    float searchSkip = 0;

    static constexpr int SPAN = (int)(WIN * 1.08f) + 3;  // muestras que abarca una ventana con escala máxima

    void resetSearch()
    {
        bestR = 1e9f;
        bestB = -1;
    }

    float content(int a) const
    {
        float c = 0;
        for (int i = 0; i < WIN - 1; i++) c += fabsf((trk.th[a + i + 1] - trk.th[a + i]) * 0.01f);
        return c;
    }

    // Error entre la ventana que empieza en a (entero) y la que empieza en b (fraccional) a escala s.
    float rmsAt(int a, float b, float s) const
    {
        float ta = trk.th[a] * 0.01f, tb = trk.thAt(b);
        float acc = 0;
        for (int i = 0; i < WIN; i++) {
            float da = trk.th[a + i] * 0.01f - ta;
            float db = trk.thAt(b + i * s) - tb;
            float e = da - db;
            acc += e * e;
        }
        return sqrtf(acc / WIN);
    }

    float bestScaleRms(int a, float b, float &scaleOut) const
    {
        static const float SCALES[5] = {0.92f, 0.96f, 1.0f, 1.04f, 1.08f};
        float best = 1e9f;
        for (float s : SCALES) {
            float r = rmsAt(a, b, s);
            if (r < best) best = r, scaleOut = s;
        }
        return best;
    }

    // Mínimo fraccional por parábola entre b-1, b, b+1 (a escala s).
    float refine(int a, int b, float s) const
    {
        float r0 = rmsAt(a, b - 1, s), r1 = rmsAt(a, b, s), r2 = rmsAt(a, b + 1, s);
        float den = r0 - 2 * r1 + r2;
        if (den <= 1e-6f) return (float)b;
        float d = 0.5f * (r0 - r2) / den;
        return b + std::max(-1.f, std::min(1.f, d));
    }

    void addCrossing(float idx, std::vector<LapEvent> &out, int laps)
    {
        LapEvent e;
        e.number = (int)crossings.size();
        e.idx = idx;
        e.atMs = (uint32_t)trk.timeAt(idx);
        e.laps = laps;
        e.lapMs = crossings.empty() ? 0 : e.atMs - crossings.back().atMs;
        crossings.push_back(e);
        out.push_back(e);
    }

    // Fase A: todavía no se conoce el circuito. Busca en el pasado una ventana igual a la última.
    void acquire(int n, std::vector<LapEvent> &out)
    {
        int b = n - SPAN;
        if (content(b) < P.matchContent) return;  // recta: no dice nada, se conserva la cuenta
        float bestRms = 1e9f, sc = 1.f;
        int bestA = -1;
        for (int a = 0; a <= b - LMIN; a++) {
            float r = bestScaleRms(a, (float)b, sc);
            if (r < bestRms) bestRms = r, bestA = a, scaleHint = sc;
        }
        if (bestA < 0 || bestRms > P.matchRms) {
            candCount = 0;
            return;
        }
        int lag = b - bestA;
        if (candCount > 0 && abs(lag - candLag) <= std::max(4, lag / 25)) candCount++;
        else candCount = 1;
        candLag = lag;
        if (candCount < P.lockCount) return;

        // Confirmado. Se retrocede para ubicar el primer punto repetido del circuito.
        int a = bestA, bb = b;
        while (a > 0) {
            float r = rmsAt(a - 1, (float)(bb - 1), scaleHint);
            if (r > P.matchRms * 1.2f) break;
            a--, bb--;
        }
        // El cruce se ubica en el centro de la ventana: así los errores de escala del recorrido
        // pesan la mitad que si se tomara el comienzo.
        float c0 = a + 0.5f * WIN;
        float c1 = refine(a, bb, scaleHint) + 0.5f * WIN * scaleHint;
        addCrossing(c0, out, 1);
        addCrossing(c1, out, 1);
        L = c1 - c0;
        // La plantilla es el tramo con más curvas cerca del cruce (una recta no identifica nada).
        int span = std::max(8, (int)(L / 8));
        int lo = std::max(0, bb - span), hi = std::min(n - WIN - 1, bb + span);
        int best = std::min(bb, hi);
        float bestC = -1;
        for (int t0 = lo; t0 <= hi; t0++) {
            float c = content(t0);
            if (c > bestC) bestC = c, best = t0;
        }
        startTemplate(best, c1 - best);
        phaseB = true;
    }

    // Plantilla: ventana que empieza en `start`; el punto de cruce está `off` muestras adentro.
    void startTemplate(int start, float off)
    {
        tplA = start;
        tplOff = off;
        resetSearch();
    }

    // Fase B: busca el próximo cruce comparando con la ventana de referencia (la de la primera
    // vuelta), así el error no se acumula de vuelta en vuelta.
    void track(int n, std::vector<LapEvent> &out)
    {
        int b = n - SPAN;
        float last = crossings.back().idx + searchSkip;
        float lo = last + 0.80f * L;
        if (b < lo) return;
        if (content(tplA) >= P.matchContent) {
            float sc = 1.f;
            float r = bestScaleRms(tplA, (float)b, sc);
            if (r < bestR) bestR = r, bestB = b, bestScale = sc;
        }
        bool settled = bestB >= 0 && bestR < P.matchRms && b > bestB + 8;
        bool giveUp = b > last + 1.4f * L;
        if (settled) {
            float idx = refine(tplA, bestB, bestScale) + tplOff * bestScale;
            float gap = idx - crossings.back().idx;
            int laps = std::max(1, (int)lroundf(gap / L));
            addCrossing(idx, out, laps);
            if (laps == 1) L = 0.7f * L + 0.3f * gap;
            searchSkip = 0;
            resetSearch();
        } else if (giveUp) {
            // No apareció: se sigue buscando una vuelta más adelante (el cruce quedará como "perdida").
            searchSkip += L;
            resetSearch();
        }
    }
};

// --- Estimador ------------------------------------------------------------------------------------
enum class Phase { Idle, Calibrating, WaitStart, Running };

class Estimator {
public:
    explicit Estimator(const Params &p = Params()) : P(p), laps(P, track) { track.reserve(MAX_SAMPLES); }

    Params P;
    Track track;
    LapDetector laps;

    Phase phase = Phase::Idle;
    float calProgress = 0;       // 0..1
    bool calFailed = false;      // se movió durante la calibración

    // Estado estimado
    float x = 0, y = 0, th = 0, v = 0, s = 0;
    float yawRate = 0;
    float latG = 0, lonG = 0;    // aceleración normal / tangencial en g (para mostrar)
    bool moving = false;

    void startCalibration()
    {
        phase = Phase::Calibrating;
        calN = 0;
        calG = calA = V3();
        hfCal = 0;
        calFailed = false;
        calProgress = 0;
    }

    void reset()
    {
        phase = Phase::Idle;
        x = y = th = v = s = 0;
        track.clear();
        laps.reset();
    }

    // Una muestra: a en g, w en °/s (cuerpo), t en ms. Devuelve los cruces nuevos en `events`.
    void update(const V3 &a, const V3 &wdps, uint32_t t, std::vector<LapEvent> &events)
    {
        float dt = lastT ? (t - lastT) * 1e-3f : 0.01f;
        lastT = t;
        if (dt <= 0 || dt > 0.1f) dt = 0.01f;

        switch (phase) {
            case Phase::Idle: return;
            case Phase::Calibrating: calibrate(a, wdps); return;
            default: break;
        }

        const float D2R = PI_F / 180.f;
        V3 w((wdps.x - gyroBias.x) * D2R, (wdps.y - gyroBias.y) * D2R, (wdps.z - gyroBias.z) * D2R);

        // Filtro pasabajos en cascada de la aceleración (saca la vibración del motor).
        float al = dt / (dt + 1.f / (2 * PI_F * P.accLpHz));
        aLp1.x += al * (a.x - aLp1.x), aLp1.y += al * (a.y - aLp1.y), aLp1.z += al * (a.z - aLp1.z);
        {
            float d = fabsf(a.x - aLp1.x) + fabsf(a.y - aLp1.y) + fabsf(a.z - aLp1.z);
            hf += (d - hf) * std::min(1.f, dt / 0.3f);
        }
        aLp.x += al * (aLp1.x - aLp.x), aLp.y += al * (aLp1.y - aLp.y), aLp.z += al * (aLp1.z - aLp.z);

        V3 ae = att.toEarth(aLp);
        ae.z -= 1.f;  // sin gravedad
        float ah = sqrtf(ae.x * ae.x + ae.y * ae.y);

        // Actitud: corrige con la gravedad solo cuando lo que se mide, filtrado fuerte, es casi solo
        // gravedad (sin aceleración sostenida).
        float as = dt / (dt + 1.f / (2 * PI_F * P.attLpHz));
        aSlow.x += as * (a.x - aSlow.x), aSlow.y += as * (a.y - aSlow.y), aSlow.z += as * (a.z - aSlow.z);
        float an = norm(aSlow);
        V3 aN(aSlow.x / an, aSlow.y / an, aSlow.z / an);
        V3 aeSlow = att.toEarth(aSlow);
        float ahSlow = sqrtf(aeSlow.x * aeSlow.x + aeSlow.y * aeSlow.y);
        bool trust = fabsf(an - 1.f) < P.attGate && ahSlow < P.attGate && ah < 3 * P.attGate;
        att.update(w, trust ? &aN : nullptr, P.attKp, dt);

        V3 up = att.up();
        float wz = up.x * w.x + up.y * w.y + up.z * w.z;  // giro vertical, rad/s
        yawRate = wz;

        float gm = sqrtf(w.x * w.x + w.y * w.y + w.z * w.z) / D2R;
        gyroEma += (gm - gyroEma) * std::min(1.f, dt / 0.3f);

        if (phase == Phase::WaitStart) {
            waitStart(ae, ah, wz, dt, t);
            return;
        }
        run(ae, ah, wz, dt, t, events);
    }

    float speedKmh() const { return v * 3.6f; }
    V3 upBody() const { return att.up(); }
    float dbgBa() const { return ba; }

private:
    int calN = 0;
    V3 calG, calA, calPrev;
    float hfCal = 0;
    V3 gyroBias;
    Attitude att;
    uint32_t lastT = 0;
    V3 aLp, aLp1, aSlow;
    float gyroEma = 0, hf = 0, hf0 = 0;
    // arranque
    float accX = 0, accY = 0, quietT = 0;
    // filtro plano
    float Pv = 1, ba = 0, bYaw = 0, stillT = 0, vibT = 0;
    float nextS = 0;

    void calibrate(const V3 &a, const V3 &wdps)
    {
        if (calN > 0) {
            V3 m(calG.x / calN, calG.y / calN, calG.z / calN);
            if (fabsf(wdps.x - m.x) > 6 || fabsf(wdps.y - m.y) > 6 || fabsf(wdps.z - m.z) > 6) {
                calN = 0, calG = calA = V3();
                hfCal = 0;
                calFailed = true;
            }
        }
        {
            float d = fabsf(a.x - calPrev.x) + fabsf(a.y - calPrev.y) + fabsf(a.z - calPrev.z);
            if (calN > 0) hfCal += d * 0.5f;  // la diferencia entre muestras sobreestima x2 la vibración
            calPrev = a;
        }
        calG.x += wdps.x, calG.y += wdps.y, calG.z += wdps.z;
        calA.x += a.x, calA.y += a.y, calA.z += a.z;
        calN++;
        calProgress = std::min(1.f, calN / 200.f);
        if (calN < 200) return;

        gyroBias = V3(calG.x / calN, calG.y / calN, calG.z / calN);
        V3 am(calA.x / calN, calA.y / calN, calA.z / calN);
        float n = norm(am);
        if (n < 0.8f || n > 1.2f) {  // no es gravedad: estaba en movimiento
            calN = 0, calG = calA = V3();
            calFailed = true;
            return;
        }
        att.initFromUp(V3(am.x / n, am.y / n, am.z / n));
        aLp = aLp1 = aSlow = am;
        hf0 = hfCal / std::max(1, calN);
        accX = accY = quietT = 0;
        x = y = th = v = s = 0;
        Pv = 1, ba = 0, bYaw = 0, stillT = 0, nextS = 0;
        calFailed = false;
        phase = Phase::WaitStart;
    }

    // Esperando el arranque: acumula la aceleración horizontal sostenida en una dirección.
    void waitStart(const V3 &ae, float ah, float wz, float dt, uint32_t t)
    {
        const float D2R = PI_F / 180.f;
        (void)D2R;
        if (ah > P.startAccG) {
            accX += ae.x * dt * G;
            accY += ae.y * dt * G;
            quietT = 0;
        } else {
            quietT += dt;
            if (quietT > 0.6f) accX = accY = 0;
        }
        (void)wz;
        float sp = sqrtf(accX * accX + accY * accY);
        v = sp;
        if (sp >= P.startSpeed && gyroEma < 40.f) {
            th = atan2f(accY, accX);  // la marcha empieza hacia donde aceleró
            x = y = s = 0;
            nextS = 0;
            Pv = 1;
            phase = Phase::Running;
            track.clear();
            laps.reset();
            startedAt = t;
            moving = true;
        }
    }

    void run(const V3 &ae, float ah, float wzRaw, float dt, uint32_t t, std::vector<LapEvent> &events)
    {
        float wz = wzRaw - bYaw;
        th += wz * dt;
        float ct = cosf(th), st = sinf(th);
        float at = (ae.x * ct + ae.y * st) * G - ba;
        float anrm = (-ae.x * st + ae.y * ct) * G;
        lonG = at / G;
        latG = anrm / G;

        // Quieto: velocidad cero y ajuste fino del sesgo vertical del giróscopo.
        // Quieto: sin giro ni aceleración sostenida y, o bien va lento, o bien vibra como en reposo
        // (si el frenazo de un choque no se integró bien, la vibración dice que ya está parado).
        bool calm = gyroEma < P.stillGyroDps && ah < P.stillAccG;
        bool slow = v < P.zuptMaxSpeed;
        bool quietVib = hf0 > 0 && hf < P.vibFactor * hf0;
        stillT = (calm && slow) ? stillT + dt : 0;
        vibT = (calm && quietVib) ? vibT + dt : 0;
        if (stillT > 0.5f && !quietVib) hf0 += 0.001f * (hf - hf0);  // sigue el ruido de reposo (motor en ralentí)
        if (stillT > 0.5f || vibT > P.vibStillSec) {
            v = 0, Pv = 0.01f;
            bYaw += 0.02f * (wzRaw - bYaw);
            moving = false;
        } else {
            moving = true;
            v += at * dt;
            Pv += P.speedQ * dt;
            if (v < 0) v = 0;
            if (fabsf(wz) > P.cornerMinW && v > 0.5f) {
                float z = anrm / wz;  // en curva: a_n = v·ω
                if (z > 0 && z < 45.f) {
                    float R = (P.cornerSigma / wz) * (P.cornerSigma / wz);
                    float K = Pv / (Pv + R);
                    float nu = z - v;
                    v += K * nu;
                    Pv *= (1 - K);
                    ba -= P.biasGain * nu * dt;
                    ba = std::max(-0.8f, std::min(0.8f, ba));
                }
            }
        }

        float ds = v * dt;
        x += ds * ct;
        y += ds * st;
        s += ds;

        while (s >= nextS && !track.full()) {
            track.push(th, t, x, y, v);
            nextS += DS;
            laps.update(events);
        }
    }

public:
    uint32_t startedAt = 0;
};

}  // namespace kart
