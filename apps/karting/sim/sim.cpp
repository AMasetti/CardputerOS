// Simulador de karting para probar kart_core.h en la PC.
//   g++ -std=gnu++11 -O2 -I../src sim.cpp -o sim && ./sim [semilla] [salida.csv]
// Genera un circuito cerrado, un kart que lo recorre (1 vuelta de reconocimiento + 10) y las
// lecturas de un IMU en un bolsillo (orientación arbitraria, vibración, sesgos, ruido).
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "kart_core.h"

using namespace kart;

struct Mat {
    float m[3][3];
};
static Mat rotXYZ(float rx, float ry, float rz)
{
    float cx = cosf(rx), sx = sinf(rx), cy = cosf(ry), sy = sinf(ry), cz = cosf(rz), sz = sinf(rz);
    Mat r = {{{cy * cz, -cy * sz, sy},
              {sx * sy * cz + cx * sz, -sx * sy * sz + cx * cz, -sx * cy},
              {-cx * sy * cz + sx * sz, cx * sy * sz + sx * cz, cx * cy}}};
    return r;
}
static V3 mulT(const Mat &r, const V3 &v)  // r^T * v  (mundo -> cuerpo)
{
    return V3(r.m[0][0] * v.x + r.m[1][0] * v.y + r.m[2][0] * v.z, r.m[0][1] * v.x + r.m[1][1] * v.y + r.m[2][1] * v.z,
              r.m[0][2] * v.x + r.m[1][2] * v.y + r.m[2][2] * v.z);
}

static float vibScale = 1;
int main(int argc, char **argv)
{
    unsigned seed = argc > 1 ? atoi(argv[1]) : 1;
    const char *outPath = argc > 2 ? argv[2] : nullptr;
    std::mt19937 rng(seed);
    std::normal_distribution<float> N(0, 1);
    std::uniform_real_distribution<float> U(-1, 1);

    // --- Circuito: curva cerrada, remuestreada por distancia ---
    const int NP = 200000;
    std::vector<float> cx(NP), cy(NP);
    for (int i = 0; i < NP; i++) {
        float p = 2 * PI_F * i / NP;
        cx[i] = 95 * cosf(p) + 20 * cosf(2 * p + 0.5f) + 12 * sinf(3 * p);
        cy[i] = 55 * sinf(p) + 14 * sinf(2 * p) + 9 * cosf(3 * p + 1.f);
    }
    std::vector<float> sArr(NP + 1, 0);
    for (int i = 0; i < NP; i++) {
        int j = (i + 1) % NP;
        sArr[i + 1] = sArr[i] + hypotf(cx[j] - cx[i], cy[j] - cy[i]);
    }
    float Ltrack = sArr[NP];
    const float h = 0.25f;
    int M = (int)(Ltrack / h);
    std::vector<float> X(M), Y(M), TH(M), KAP(M), VP(M);
    int k = 0;
    for (int i = 0; i < M; i++) {
        float s = i * h;
        while (sArr[k + 1] < s) k++;
        float f = (s - sArr[k]) / (sArr[k + 1] - sArr[k]);
        int j = (k + 1) % NP;
        X[i] = cx[k] + (cx[j] - cx[k]) * f;
        Y[i] = cy[k] + (cy[j] - cy[k]) * f;
    }
    for (int i = 0; i < M; i++) {
        int a = (i + M - 4) % M, b = (i + 4) % M;
        TH[i] = atan2f(Y[b] - Y[a], X[b] - X[a]);
    }
    for (int i = 0; i < M; i++) {
        int a = (i + M - 8) % M, b = (i + 8) % M;
        float d = TH[b] - TH[a];
        while (d > PI_F) d -= 2 * PI_F;
        while (d < -PI_F) d += 2 * PI_F;
        KAP[i] = d / (16 * h);
    }
    // Perfil de velocidad: límite lateral y límites de aceleración/frenada.
    for (int i = 0; i < M; i++) VP[i] = std::min(22.f, sqrtf(8.0f / std::max(1e-3f, fabsf(KAP[i]))));
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < 2 * M; i++) {
            int a = i % M, b = (i + 1) % M;
            VP[b] = std::min(VP[b], sqrtf(VP[a] * VP[a] + 2 * 4.0f * h));
        }
        for (int i = 2 * M; i > 0; i--) {
            int a = i % M, b = (i + M - 1) % M;
            VP[b] = std::min(VP[b], sqrtf(VP[a] * VP[a] + 2 * 7.0f * h));
        }
    }
    printf("circuito: %.0f m, vmax %.1f m/s\n", Ltrack, *std::max_element(VP.begin(), VP.end()));

    // --- Sensor en el bolsillo ---
    Mat Rbase = rotXYZ(0.4f * U(rng) * 2, 0.9f * U(rng), 3.0f * U(rng));  // cuerpo->mundo del kart
    if (getenv("NOBIAS")) {}
    V3 gBias(0.5f * N(rng) * 0.4f, 0.5f * N(rng) * 0.4f, 0.5f * N(rng) * 0.4f);  // °/s
    V3 aBias(0.02f * N(rng), 0.02f * N(rng), 0.02f * N(rng));

    if (getenv("NOROT")) Rbase = rotXYZ(0, 0, getenv("YAW") ? atof(getenv("YAW")) : 0);
    if (getenv("NOBIAS")) gBias = V3(), aBias = V3();
    Params P;
    if (getenv("ZUPT0")) P.zuptMaxSpeed = 0;
    if (getenv("NOCORNER")) P.cornerMinW = 99;
    if (getenv("ATTKP")) P.attKp = atof(getenv("ATTKP"));
    if (getenv("ATTG")) P.attGate = atof(getenv("ATTG"));
    if (getenv("BG")) P.biasGain = atof(getenv("BG"));
    if (getenv("CS")) P.cornerSigma = atof(getenv("CS"));
    if (getenv("SQ")) P.speedQ = atof(getenv("SQ"));
    if (getenv("LP")) P.accLpHz = atof(getenv("LP"));
    if (getenv("NONOISE")) { vibScale = 0; }
    Estimator est(P);
    std::vector<LapEvent> events;
    est.startCalibration();

    const float dt = 0.01f;
    uint32_t tms = 0;
    const int RACE_LAPS = 10, TOTAL = 1 + RACE_LAPS;

    // Truth
    std::vector<uint32_t> truthCross;  // instante en que s verdadera cruza k*Ltrack
    float sTrue = 0, vTrue = 0;
    float prevV = 0;
    float lapScale = 1.f;
    int lapIdx = 0;
    bool started = false;
    double vErr2 = 0; long vN = 0; double zuptFrac = 0;
    std::vector<float> sAt;  // distancia verdadera cada 10 ms

    FILE *out = outPath ? fopen(outPath, "w") : nullptr;
    if (out) fprintf(out, "t,x,y,truex,truey,v\n");
    float estOffs = 0;
    (void)estOffs;

    for (int step = 0; step < 200000; step++, tms += 10) {
        float t = step * dt;
        // Quieto 6 s (2,5 de calibración + espera), después sale desde la línea.
        sAt.push_back(sTrue);
        float aT = 0, aN = 0, wz = 0, th = 0;
        if (t >= 6.0f && !getenv("NOGO")) {
            if (!started) started = true;
            static int crashLeft = 0; static bool crashed = false, crashBrake = false;
            if (getenv("CRASH") && !crashed && sTrue > 3.4f * Ltrack) { crashed = true; crashLeft = 1500; crashBrake = true; }
            int i = ((int)(sTrue / h)) % M;
            float target = VP[i] * lapScale;
            float acc = std::max(-7.f, std::min(4.f, (target - vTrue) * 2.0f));
            if (crashBrake) { acc = -120.f; if (vTrue <= 0.5f) { crashBrake = false; vTrue = 0; } }
            else if (crashLeft > 0) { crashLeft--; acc = 0; vTrue = 0; }
            vTrue = std::max(0.f, vTrue + acc * dt);
            aT = (vTrue - prevV) / dt;
            prevV = vTrue;
            sTrue += vTrue * dt;
            if (sTrue >= (lapIdx + 1) * Ltrack) {
                truthCross.push_back(tms);
                if (getenv("DBG")) printf("[dbg] t=%.1f lap %d: s_est=%.0f s_true=%.0f  th_est=%.2f v_est=%.1f v_true=%.1f\n", tms / 1000.0, lapIdx + 1, est.s, sTrue, est.th, est.v, vTrue);
                lapIdx++;
                lapScale = 0.96f + 0.06f * (U(rng) * 0.5f + 0.5f);
                if (lapIdx >= TOTAL + 1) break;
            }
            i = ((int)(sTrue / h)) % M;
            aN = vTrue * vTrue * KAP[i];
            wz = vTrue * KAP[i];
            th = TH[i];
            if (getenv("TRACE") && step % 25 == 0) {
                V3 tu = mulT(Rbase, V3(0, 0, 1)), eu = est.upBody();
                float c = tu.x * eu.x + tu.y * eu.y + tu.z * eu.z;
                float dth = est.th - th; while (dth > PI_F) dth -= 2 * PI_F; while (dth < -PI_F) dth += 2 * PI_F;
                printf("ba=%.2f dth=%.2f tilt=%.1f t=%.1f vt=%.1f ve=%.1f wz_t=%.2f wz_e=%.2f aN_t=%.2f latG_e=%.2f aT_t=%.2f lonG_e=%.2f\n", est.dbgBa(), dth, acosf(std::min(1.f, c)) * 57.3f, t,
                       vTrue, est.v, wz, est.yawRate, aN / G, est.latG, aT / G, est.lonG);
            }
            if (out && step % 10 == 0) fprintf(out, "%u,%.2f,%.2f,%.2f,%.2f,%.2f\n", tms, est.x, est.y, X[i], Y[i], vTrue);
        }
        // Aceleración y giro en el marco del kart (g, rad/s)
        float ct = cosf(th), st = sinf(th);
        (void)ct, (void)st;
        V3 fW(aT / G, aN / G, 1.f);  // marco del kart: x adelante, y a la izquierda, z arriba
        V3 wW(0, 0, wz);
        bool moving = vTrue > 0.2f;
        float vib = (moving ? 0.12f : 0.05f) * vibScale;
        V3 vibe(vib * N(rng), vib * N(rng), vib * 1.3f * N(rng));
        // El cuerpo se mueve un poco: bamboleo lento
        float wob = (getenv("NOWOB") || t < 5.5f) ? 0.f : 1.f;
        Mat R = rotXYZ(wob * 0.05f * sinf(t * 0.7f), wob * 0.05f * sinf(t * 0.5f + 1), 0);
        // Rbody = Rbase * wobble  (se aproxima aplicando el bamboleo al mundo)
        V3 fK = mulT(R, fW);
        V3 aB = mulT(Rbase, fK);
        V3 wK = mulT(R, wW);
        // el bamboleo también es una rotación que el giróscopo ve (ángulos chicos)
        {
            float tn = t + 0.001f;
            Mat Rn = rotXYZ(wob * 0.05f * sinf(tn * 0.7f), wob * 0.05f * sinf(tn * 0.5f + 1), 0);
            // S = R^T (Rn - R) / dt  (antisimétrica)
            float S[3][3];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    S[i][j] = 0;
                    for (int k2 = 0; k2 < 3; k2++) S[i][j] += R.m[k2][i] * (Rn.m[k2][j] - R.m[k2][j]) / 0.001f;
                }
            wK.x += S[2][1];
            wK.y += S[0][2];
            wK.z += S[1][0];
        }
        V3 wB = mulT(Rbase, wK);
        float dps = 180.f / PI_F;
        V3 a(aB.x + vibe.x + aBias.x + 0.02f * N(rng), aB.y + vibe.y + aBias.y + 0.02f * N(rng),
             aB.z + vibe.z + aBias.z + 0.02f * N(rng));
        V3 w(wB.x * dps + gBias.x + 0.4f * N(rng), wB.y * dps + gBias.y + 0.4f * N(rng), wB.z * dps + gBias.z + 0.4f * N(rng));

        est.update(a, w, tms, events);
        if (getenv("NOGO") && step % 300 == 0 && step < 3000) { V3 tu = mulT(Rbase, mulT(R, V3(0, 0, 1))), eu = est.upBody(); float c = tu.x * eu.x + tu.y * eu.y + tu.z * eu.z; printf("idle t=%.1f tilt=%.2f deg phase=%d\n", t, acosf(std::min(1.f, c)) * 57.3f, (int)est.phase); }
        if (est.phase == Phase::Running && vTrue > 0) { double e = est.v - vTrue; vErr2 += e * e; vN++; static int shown = 0; if (getenv("FIRSTERR") && fabs(e) > 3 && shown < 6 && step % 20 == 0) { shown++; printf("[err] t=%.2f vt=%.1f ve=%.1f s_true=%.0f s_est=%.0f\n", t, vTrue, est.v, sTrue, est.s); } }
        if (getenv("AE") && step % 50 == 0 && t > 6) printf("t=%.1f fW=(%.2f,%.2f) a_body=(%.2f,%.2f,%.2f) th=%.2f est_th=%.2f\n", t, fW.x, fW.y, a.x, a.y, a.z, th, est.th);
        if (est.phase == Phase::Calibrating && est.calProgress >= 1.f) {}
        if (est.phase == Phase::Idle) break;
    }

    // --- Resultados ---
    printf("fase final=%d, cruces detectados=%zu, vueltas verdaderas=%zu\n", (int)est.phase, est.laps.crossings.size(), truthCross.size());
    printf("\n  #  detectado(s)  vuelta(s)  punto(m)  desvio(m)  tipo\n");
    std::vector<float> ph;
    for (auto &e : est.laps.crossings) {
        size_t k = std::min(sAt.size() - 1, (size_t)(e.atMs / 10));
        ph.push_back(fmodf(sAt[k], Ltrack));
    }
    std::vector<float> sorted = ph;
    std::sort(sorted.begin(), sorted.end());
    float med = sorted.empty() ? 0 : sorted[sorted.size() / 2];
    double sumsq = 0;
    int cnt = 0;
    for (size_t i = 0; i < est.laps.crossings.size(); i++) {
        const LapEvent &e = est.laps.crossings[i];
        float dev = ph[i] - med;
        if (dev > Ltrack / 2) dev -= Ltrack;
        if (dev < -Ltrack / 2) dev += Ltrack;
        printf("%3zu  %11.2f  %9.2f  %8.1f  %9.1f  %s\n", i, e.atMs / 1000.0, e.lapMs / 1000.0, ph[i], dev, e.laps > 1 ? "perdida" : "");
        if (e.laps == 1) sumsq += dev * dev, cnt++;
    }
    printf("\nerror RMS de velocidad: %.2f m/s, distancia estimada/real: %.3f\n", sqrt(vErr2 / std::max(1L, vN)), est.s / std::max(1.f, sTrue));
    printf("\ndesvio RMS del punto de cruce: %.1f m (~%.0f ms a 17 m/s)\n", sqrt(sumsq / std::max(1, cnt)), sqrt(sumsq / std::max(1, cnt)) / 17 * 1000);
    printf("vueltas verdaderas: ");
    for (size_t j = 1; j < truthCross.size(); j++) printf("%.2f ", (truthCross[j] - truthCross[j - 1]) / 1000.0);
    printf("\n");
    if (out) fclose(out);
    return 0;
}
