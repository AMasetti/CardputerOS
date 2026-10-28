// Brújula para M5Stack Cardputer-Adv
//
// El Cardputer-Adv trae un BMI270 (acelerómetro + giróscopo) pero no tiene
// magnetómetro, así que no puede encontrar el norte solo: lo marcás una vez
// apuntando el borde de arriba hacia el norte y apretando enter, y desde ahí
// el giróscopo sigue el rumbo. El acelerómetro corrige la inclinación (filtro
// de Mahony), así que la orientación en 3D no deriva; el rumbo sí deriva un
// poco con el tiempo y conviene volver a fijarlo de vez en cuando.
//
//   tab  m        vista 3D / 2D
//   enter         fijar el norte hacia donde apunta el Cardputer
//   ,  /          girar el norte 5° a la izquierda / derecha
//   c             recalibrar el giróscopo (dejalo quieto)
//   x             cambiar la orientación del sensor si los ejes no coinciden
//
// La vista y la orientación del sensor se guardan en la flash (NVS).

#include <M5Cardputer.h>
#include <Preferences.h>
#include <math.h>
#include <algorithm>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static constexpr float DEG = 180.0f / M_PI;
static constexpr float RAD = M_PI / 180.0f;

// --- Filtro de Mahony ---------------------------------------------------------
// q lleva del cuerpo (X derecha, Y hacia el borde de arriba, Z saliendo de la
// pantalla) al mundo (Z hacia arriba). Solo el acelerómetro corrige, así que
// el error nunca tiene componente vertical: no toca el rumbo.
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

// Matriz de rotación cuerpo -> mundo.
struct Mat {
    float m[3][3];
};

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

// --- Estado ---------------------------------------------------------------------
enum class View : uint8_t { ThreeD, TwoD };

static Preferences prefs;
static View view = View::ThreeD;
static uint8_t axisMap = 0;     // 0..7: giro de 90° en Z y volteo
static float northOffset = 0;   // rumbo crudo que corresponde al norte (rad)
static bool northSet = false;

static float bias[3] = {0, 0, 0}; // sesgo del giróscopo (°/s)
static bool calibrating = true;
static int calN = 0;
static float calSum[3], calAcc[3];
static constexpr int CAL_SAMPLES = 150;
static uint32_t stillSince = 0;
static uint32_t lastUs = 0;

static float heading = 0;         // rad, horario desde el norte
static float pitchDeg = 0, rollDeg = 0;
static Mat R;

static char toast[48] = "";
static uint32_t toastUntil = 0;

static M5Canvas spr(&M5Cardputer.Display);

static const uint16_t C_BG = lgfx::color565(8, 12, 20);
static const uint16_t C_PANEL = lgfx::color565(26, 32, 46);
static const uint16_t C_DIM = lgfx::color565(100, 110, 130);
static const uint16_t C_TEXT = lgfx::color565(230, 235, 245);
static const uint16_t C_RED = lgfx::color565(255, 70, 60);
static const uint16_t C_GREEN = lgfx::color565(70, 225, 120);
static const uint16_t C_YELLOW = lgfx::color565(255, 205, 50);
static const uint16_t C_ACCENT = lgfx::color565(80, 170, 255);

static const char *CARDINAL[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                   "S", "SSO", "SO", "OSO", "O", "ONO", "NO", "NNO"};

static float wrap(float a)
{
    while (a < 0) a += 2 * M_PI;
    while (a >= 2 * M_PI) a -= 2 * M_PI;
    return a;
}

static void showToast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toastUntil = millis() + 1800;
}

static void savePrefs()
{
    prefs.putUChar("view", (uint8_t)view);
    prefs.putUChar("axis", axisMap);
}

// --- Sensor -----------------------------------------------------------------------
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
    memset(calAcc, 0, sizeof(calAcc));
    kp = 8.0f; // converge rápido a la gravedad mientras está quieto
}

// Rumbo crudo: hacia dónde apunta el borde de arriba, o la parte de atrás
// cuando lo tenés parado mirando la pantalla. La suma de las dos
// proyecciones horizontales pasa suave de un caso al otro.
static float rawHeading(const Mat &r)
{
    float fx = r.m[0][1] - r.m[0][2];
    float fy = r.m[1][1] - r.m[1][2];
    return atan2f(fx, fy);
}

static void readImu()
{
    if (!M5.Imu.update()) return;
    float ax, ay, az, gx, gy, gz;
    M5.Imu.getAccel(&ax, &ay, &az);
    M5.Imu.getGyro(&gx, &gy, &gz);
    mapAxes(ax, ay, az);
    mapAxes(gx, gy, gz);

    uint32_t now = micros();
    float dt = lastUs ? (now - lastUs) * 1e-6f : 0.01f;
    lastUs = now;
    if (dt > 0.1f) dt = 0.1f;

    if (calibrating) {
        // Si se mueve, empieza de nuevo.
        if (calN > 0) {
            float mg = 0;
            for (int i = 0; i < 3; ++i) mg = std::max(mg, fabsf((i == 0 ? gx : i == 1 ? gy : gz) - calSum[i] / calN));
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
    heading = wrap(rawHeading(R) - northOffset);
    pitchDeg = asinf(std::max(-1.0f, std::min(1.0f, R.m[2][1]))) * DEG;
    rollDeg = asinf(std::max(-1.0f, std::min(1.0f, R.m[2][0]))) * DEG;
}

// --- Vista 3D ---------------------------------------------------------------------
// Cámara detrás del Cardputer y un poco arriba, que gira con el rumbo: el
// Cardputer siempre apunta "hacia adentro" y la rosa gira debajo.
static constexpr float CAM_E = 0.62f;  // elevación (rad)
static constexpr float CAM_D = 3.4f;
static constexpr float CAM_F = 120.0f;
static constexpr int CX = 120, CY = 78;
static constexpr float GROUND = -0.5f;

struct P2 {
    float x, y, depth;
};

// Punto ya rotado al marco de la cámara (xr a la derecha, yr hacia adelante).
static P2 project(float xr, float yr, float z)
{
    float ce = cosf(CAM_E), se = sinf(CAM_E);
    float up = yr * se + z * ce;
    float depth = CAM_D + yr * ce - z * se;
    return {CX + CAM_F * xr / depth, CY - CAM_F * up / depth, depth};
}

// Un rumbo absoluto (b, rad) a distancia r sobre el piso.
static P2 groundPoint(float b, float r)
{
    float a = b - heading;
    return project(r * sinf(a), r * cosf(a), GROUND);
}

static void drawRose()
{
    // Anillo y marcas.
    const float r = 1.25f;
    P2 prev = groundPoint(0, r);
    for (int i = 1; i <= 72; ++i) {
        P2 p = groundPoint(i * 5 * RAD, r);
        spr.drawLine(prev.x, prev.y, p.x, p.y, C_DIM);
        prev = p;
    }
    for (int d = 0; d < 360; d += 15) {
        float len = d % 90 == 0 ? 0.2f : (d % 45 == 0 ? 0.13f : 0.07f);
        P2 a = groundPoint(d * RAD, r), b = groundPoint(d * RAD, r - len);
        spr.drawLine(a.x, a.y, b.x, b.y, d % 90 == 0 ? C_TEXT : C_DIM);
    }

    // Aguja: roja al norte, blanca al sur.
    P2 c = groundPoint(0, 0);
    P2 n = groundPoint(0, 1.0f), s = groundPoint(M_PI, 1.0f);
    P2 l = groundPoint(-M_PI / 2, 0.12f), rr = groundPoint(M_PI / 2, 0.12f);
    spr.fillTriangle(n.x, n.y, l.x, l.y, rr.x, rr.y, C_RED);
    spr.fillTriangle(s.x, s.y, l.x, l.y, rr.x, rr.y, lgfx::color565(200, 205, 215));
    spr.fillCircle(c.x, c.y, 2, C_PANEL);

    // Letras.
    spr.setFont(&fonts::DejaVu18);
    spr.setTextDatum(textdatum_t::middle_center);
    const char *L[4] = {"N", "E", "S", "O"};
    for (int i = 0; i < 4; ++i) {
        P2 p = groundPoint(i * M_PI / 2, r + 0.3f);
        spr.setTextColor(i == 0 ? C_RED : C_TEXT);
        spr.drawString(L[i], p.x, p.y);
    }
}

static void drawBox()
{
    // Medidas del Cardputer, a escala: ancho, alto y espesor.
    const float hx = 0.55f, hy = 0.34f, hz = 0.1f;
    float raw = wrap(heading + northOffset);
    float cs = cosf(raw), sn = sinf(raw);
    P2 pts[8];
    float world[8][3];
    for (int i = 0; i < 8; ++i) {
        float b[3] = {(i & 1) ? hx : -hx, (i & 2) ? hy : -hy, (i & 4) ? hz : -hz};
        float w[3];
        for (int k = 0; k < 3; ++k) w[k] = R.m[k][0] * b[0] + R.m[k][1] * b[1] + R.m[k][2] * b[2];
        memcpy(world[i], w, sizeof(w));
        float xr = w[0] * cs - w[1] * sn;
        float yr = w[0] * sn + w[1] * cs;
        pts[i] = project(xr, yr, w[2]);
    }

    struct Face {
        uint8_t v[4];
        int8_t axis, sign; // normal en el cuerpo
        float depth;
    };
    Face faces[6] = {
        {{1, 3, 7, 5}, 0, 1, 0}, {{0, 4, 6, 2}, 0, -1, 0}, {{2, 6, 7, 3}, 1, 1, 0},
        {{0, 1, 5, 4}, 1, -1, 0}, {{4, 5, 7, 6}, 2, 1, 0}, {{0, 2, 3, 1}, 2, -1, 0},
    };
    for (auto &f : faces) {
        f.depth = 0;
        for (int k = 0; k < 4; ++k) f.depth += pts[f.v[k]].depth;
    }
    std::sort(faces, faces + 6, [](const Face &a, const Face &b) { return a.depth > b.depth; });

    for (auto &f : faces) {
        // Luz de arriba: cuanto más mira la cara hacia arriba, más clara.
        float nz = R.m[2][f.axis] * f.sign;
        float light = 0.45f + 0.55f * std::max(0.0f, nz);
        uint8_t base[3] = {70, 78, 92};
        if (f.axis == 2 && f.sign > 0) base[0] = 40, base[1] = 60, base[2] = 110; // pantalla
        if (f.axis == 1 && f.sign > 0) base[0] = 230, base[1] = 110, base[2] = 40; // borde de arriba
        uint16_t col = lgfx::color565(base[0] * light, base[1] * light, base[2] * light);
        const P2 &a = pts[f.v[0]], &b = pts[f.v[1]], &c = pts[f.v[2]], &d = pts[f.v[3]];
        spr.fillTriangle(a.x, a.y, b.x, b.y, c.x, c.y, col);
        spr.fillTriangle(a.x, a.y, c.x, c.y, d.x, d.y, col);
        spr.drawLine(a.x, a.y, b.x, b.y, C_BG);
        spr.drawLine(b.x, b.y, c.x, c.y, C_BG);
        spr.drawLine(c.x, c.y, d.x, d.y, C_BG);
        spr.drawLine(d.x, d.y, a.x, a.y, C_BG);
    }
}

static void render3d()
{
    drawRose();
    drawBox();

    // Rumbo arriba a la izquierda.
    char buf[24];
    int deg = (int)lroundf(heading * DEG) % 360;
    spr.setFont(&fonts::DejaVu24);
    spr.setTextDatum(textdatum_t::top_left);
    spr.setTextColor(northSet ? C_TEXT : C_YELLOW);
    snprintf(buf, sizeof(buf), "%d", deg);
    spr.drawString(buf, 4, 3);
    int w = spr.textWidth(buf);
    spr.drawCircle(4 + w + 4, 7, 2, northSet ? C_TEXT : C_YELLOW);
    spr.setFont(&fonts::lgfxJapanGothicP_16);
    spr.setTextColor(C_ACCENT);
    spr.drawString(CARDINAL[(int)lroundf(heading * DEG / 22.5f) % 16], 4 + w + 10, 7);

    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextDatum(textdatum_t::top_right);
    spr.setTextColor(C_DIM);
    snprintf(buf, sizeof(buf), "pitch %+d°", (int)lroundf(pitchDeg));
    spr.drawString(buf, 236, 3);
    snprintf(buf, sizeof(buf), "roll %+d°", (int)lroundf(rollDeg));
    spr.drawString(buf, 236, 16);
}

// --- Vista 2D -------------------------------------------------------------------------
static void render2d()
{
    const int cx = 68, cy = 68, r = 60;
    float h = heading;
    auto at = [&](float b, float rad, int &x, int &y) {
        float a = b - h;
        x = cx + (int)lroundf(rad * sinf(a));
        y = cy - (int)lroundf(rad * cosf(a));
    };

    spr.fillCircle(cx, cy, r, C_PANEL);
    spr.drawCircle(cx, cy, r, C_DIM);
    for (int d = 0; d < 360; d += 10) {
        int x0, y0, x1, y1;
        int len = d % 90 == 0 ? 10 : (d % 30 == 0 ? 7 : 3);
        at(d * RAD, r - 1, x0, y0);
        at(d * RAD, r - 1 - len, x1, y1);
        spr.drawLine(x0, y0, x1, y1, d % 30 == 0 ? C_TEXT : C_DIM);
    }
    spr.setTextDatum(textdatum_t::middle_center);
    spr.setFont(&fonts::DejaVu18);
    const char *L[4] = {"N", "E", "S", "O"};
    for (int i = 0; i < 4; ++i) {
        int x, y;
        at(i * M_PI / 2, r - 22, x, y);
        spr.setTextColor(i == 0 ? C_RED : C_TEXT);
        spr.drawString(L[i], x, y);
    }
    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextColor(C_DIM);
    for (int d = 30; d < 360; d += 30) {
        if (d % 90 == 0) continue;
        int x, y;
        at(d * RAD, r - 17, x, y);
        spr.drawNumber(d / 10, x, y);
    }

    // Aguja hacia el norte.
    int nx, ny, sx, sy, lx, ly, rx, ry;
    at(0, r - 30, nx, ny);
    at(M_PI, r - 30, sx, sy);
    at(-M_PI / 2, 5, lx, ly);
    at(M_PI / 2, 5, rx, ry);
    spr.fillTriangle(nx, ny, lx, ly, rx, ry, C_RED);
    spr.fillTriangle(sx, sy, lx, ly, rx, ry, lgfx::color565(200, 205, 215));
    spr.fillCircle(cx, cy, 3, C_TEXT);

    // Marca fija arriba: hacia dónde apuntás.
    spr.fillTriangle(cx, cy - r + 6, cx - 6, cy - r - 5, cx + 6, cy - r - 5, C_ACCENT);

    // Rumbo.
    char buf[8];
    int deg = (int)lroundf(h * DEG) % 360;
    snprintf(buf, sizeof(buf), "%d", deg);
    spr.setFont(&fonts::DejaVu40);
    spr.setTextDatum(textdatum_t::middle_center);
    spr.setTextColor(northSet ? C_TEXT : C_YELLOW);
    spr.drawString(buf, 186, 28);
    int w = spr.textWidth(buf);
    spr.drawCircle(186 + w / 2 + 5, 14, 3, northSet ? C_TEXT : C_YELLOW);
    spr.setFont(&fonts::lgfxJapanGothicP_16);
    spr.setTextColor(C_ACCENT);
    spr.drawString(CARDINAL[(int)lroundf(h * DEG / 22.5f) % 16], 186, 56);

    // Nivel de burbuja: la burbuja va al lado más alto.
    const int bx = 186, by = 98, br = 24;
    float ux = R.m[2][0], uy = R.m[2][1];
    float tilt = acosf(std::max(-1.0f, std::min(1.0f, R.m[2][2]))) * DEG;
    bool level = tilt < 2.0f;
    spr.fillCircle(bx, by, br, C_PANEL);
    spr.drawCircle(bx, by, br, C_DIM);
    spr.drawCircle(bx, by, 7, level ? C_GREEN : C_DIM);
    spr.drawFastHLine(bx - br, by, 2 * br, C_BG);
    spr.drawFastVLine(bx, by - br, 2 * br, C_BG);
    float k = (br - 6) / sinf(30 * RAD); // 30° de inclinación llega al borde
    float px = ux * k, py = uy * k;
    float len = sqrtf(px * px + py * py);
    if (len > br - 6) px *= (br - 6) / len, py *= (br - 6) / len;
    if (R.m[2][2] < 0) px = py = 0; // boca abajo: no tiene sentido
    spr.fillCircle(bx + (int)px, by - (int)py, 5, level ? C_GREEN : C_YELLOW);
    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextColor(level ? C_GREEN : C_DIM);
    spr.setTextDatum(textdatum_t::middle_left);
    snprintf(buf, sizeof(buf), "%d°", (int)lroundf(tilt));
    spr.drawString(buf, bx + br + 3, by + br - 4);
}

// --- Pantalla ----------------------------------------------------------------------------
static void renderCalibration()
{
    spr.setFont(&fonts::lgfxJapanGothicP_16);
    spr.setTextDatum(textdatum_t::middle_center);
    spr.setTextColor(C_TEXT);
    spr.drawString("Calibrando el giróscopo", 120, 46);
    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextColor(C_DIM);
    spr.drawString("Dejalo quieto sobre la mesa", 120, 68);
    spr.fillRoundRect(40, 84, 160, 8, 4, C_PANEL);
    spr.fillRoundRect(40, 84, std::max(8, 160 * calN / CAL_SAMPLES), 8, 4, C_ACCENT);
    if (!M5.Imu.isEnabled()) {
        spr.setTextColor(C_RED);
        spr.drawString("No se encontró el sensor (IMU)", 120, 110);
    }
}

static void render()
{
    spr.fillSprite(C_BG);
    if (calibrating) {
        renderCalibration();
    } else {
        if (view == View::ThreeD) render3d();
        else render2d();

        spr.setFont(&fonts::lgfxJapanGothicP_12);
        spr.setTextDatum(textdatum_t::bottom_left);
        if (millis() < toastUntil) {
            spr.setTextColor(C_ACCENT);
            spr.drawString(toast, 4, 134);
        } else if (!northSet) {
            spr.setTextColor(C_YELLOW);
            spr.drawString("Apuntá al norte y apretá enter", 4, 134);
        } else if (view == View::ThreeD) {
            spr.setTextColor(C_DIM);
            spr.drawString("tab: 2D   enter: fijar norte", 4, 134);
        }
    }
    spr.pushSprite(0, 0);
}

// --- Teclado ------------------------------------------------------------------------------
static void handleKeys()
{
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto st = M5Cardputer.Keyboard.keysState();
    if (st.tab) {
        view = view == View::ThreeD ? View::TwoD : View::ThreeD;
        savePrefs();
        return;
    }
    if (st.enter && !calibrating) {
        northOffset = wrap(heading + northOffset);
        northSet = true;
        showToast("Norte fijado");
        return;
    }
    for (char c : st.word) {
        switch (c) {
        case 'm':
        case 'M':
            view = view == View::ThreeD ? View::TwoD : View::ThreeD;
            savePrefs();
            break;
        case ',':
            northOffset = wrap(northOffset - 5 * RAD);
            break;
        case '/':
            northOffset = wrap(northOffset + 5 * RAD);
            break;
        case 'c':
        case 'C':
            startCalibration();
            break;
        case 'x':
        case 'X': {
            axisMap = (axisMap + 1) % 8;
            savePrefs();
            char buf[40];
            snprintf(buf, sizeof(buf), "Orientación del sensor %d/8", axisMap + 1);
            showToast(buf);
            startCalibration();
            break;
        }
        }
    }
}

// --- Arduino ---------------------------------------------------------------------------------
void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    prefs.begin("compass", false);
    view = (View)std::min<uint8_t>(prefs.getUChar("view", 0), 1);
    axisMap = prefs.getUChar("axis", 0) % 8;

    spr.setColorDepth(16);
    spr.createSprite(240, 135);
    R = rotation();
    startCalibration();
}

void loop()
{
    M5Cardputer.update();
    handleKeys();
    readImu();

    static uint32_t lastDraw = 0;
    if (millis() - lastDraw >= 33) {
        lastDraw = millis();
        render();
    }
    delay(1);
}
