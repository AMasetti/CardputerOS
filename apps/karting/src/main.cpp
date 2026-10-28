// Telemetría de karting para M5Stack Cardputer-Adv (sin GPS, con el aparato en el bolsillo)
//
// Usa el IMU (acelerómetro + giróscopo) con un filtro de actitud y un filtro plano de velocidad
// (ver kart_core.h) para estimar el recorrido, y detecta las vueltas solas, por repetición del
// trazado del circuito. Graba en la microSD (/karting) y muestra los resultados en pantalla.
//
// Flujo:
//   1. Configuración   ; . vueltas de carrera   r vuelta de reconocimiento sí/no   enter seguir
//   2. Sentado y quieto, apretá G0: calibra gravedad y sesgo del giróscopo (2 s)
//   3. La carrera arranca sola al acelerar. Pitidos: arranque, cada vuelta, final
//   4. Una parada brusca (choque) NO termina la sesión. Termina con G0 apretado 5 s,
//      o con enter apretado 3 s
//   5. Resultados: tab cambia entre vueltas / trazado / datos
//
// SD: solo se escribe dentro de /karting, con archivos nuevos por sesión y flush cada ~1 s.
// Nunca se formatea ni se borra nada.

#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>

#include <vector>

#include "kart_core.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HUD_H = 15;

// SD en el bus SPI de la Cardputer
static constexpr int SD_SCK = 40;
static constexpr int SD_MISO = 39;
static constexpr int SD_MOSI = 14;
static constexpr int SD_CS = 12;

static constexpr uint32_t SAMPLE_MS = 10;          // 100 Hz
static constexpr uint32_t FLUSH_MS = 1000;
static constexpr uint32_t STOP_BTN_MS = 5000;      // G0 apretado para terminar
static constexpr uint32_t STOP_KEY_MS = 3000;      // enter apretado para terminar
static constexpr uint32_t AUTO_CLOSE_MS = 90000;   // terminada la carrera y quieto

enum class Screen { Setup, Calibrating, WaitStart, Running, Done };

struct LapRec {
    int number;       // 0 = reconocimiento
    uint32_t ms;
    bool estimated;   // se perdió un cruce: tiempo repartido
    float endIdx;     // posición del cruce final en las muestras
};

struct Beep {
    uint16_t freq;
    uint16_t dur;
};

static M5Canvas canvas(&M5Cardputer.Display);
static Preferences prefs;
static kart::Params params;
static kart::Estimator est(params);

static Screen screen = Screen::Setup;
static bool dirty = true;
static uint16_t C_BG, C_HUD, C_TEXT, C_DIM, C_ACCENT, C_RED, C_GREEN, C_YELLOW;

// Configuración
static int raceLaps = 10;
static bool recon = true;

// SD
static bool sdOk;
static String sdNote;
static File rawFile, lapsFile;
static bool sdFail;
static char logBuf[4096];
static size_t logLen;
static uint32_t lastFlush;
static int sessionNo;
static char rawName[40], lapsName[40], pathName[40];

// Sesión
static std::vector<LapRec> laps;
static std::vector<kart::LapEvent> evBuf;
static int circuits;       // circuitos completos desde el primer cruce
static int doneLaps;       // vueltas de carrera completadas
static bool finished;
static uint32_t finishedAt, stillSince;
static uint32_t lastSampleMs;
static float topSpeed, maxLatG;
static uint32_t stopHoldStart;
static uint32_t lastBattCheck;
static String endReason;

// Pitidos
static Beep beepQ[10];
static int beepHead, beepTail;

// --- Utilidades ------------------------------------------------------------------------
static void fmtTime(uint32_t ms, char *out, size_t n)
{
    snprintf(out, n, "%u:%02u.%02u", (unsigned)(ms / 60000), (unsigned)((ms / 1000) % 60), (unsigned)((ms % 1000) / 10));
}

static void beep(uint16_t f, uint16_t d)
{
    int next = (beepHead + 1) % 10;
    if (next == beepTail) return;
    beepQ[beepHead] = {f, d};
    beepHead = next;
}

static void beepTick()
{
    if (beepTail == beepHead || M5Cardputer.Speaker.isPlaying()) return;
    M5Cardputer.Speaker.tone(beepQ[beepTail].freq, beepQ[beepTail].dur);
    beepTail = (beepTail + 1) % 10;
}

// --- SD: escritura protegida -----------------------------------------------------------
static bool sdInit()
{
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI, 25000000)) {
        sdNote = "Sin tarjeta SD";
        return false;
    }
    if (!SD.exists("/karting") && !SD.mkdir("/karting")) {
        sdNote = "No pude crear /karting";
        return false;
    }
    uint64_t mb = SD.cardSize() / (1024 * 1024);
    sdNote = "SD " + String((unsigned long)mb) + " MB";
    return true;
}

static void sdClose()
{
    if (rawFile) {
        if (logLen && !sdFail) rawFile.write((const uint8_t *)logBuf, logLen);
        rawFile.flush();
        rawFile.close();
    }
    if (lapsFile) {
        lapsFile.flush();
        lapsFile.close();
    }
    logLen = 0;
}

static void sdOpenSession()
{
    sdFail = false;
    logLen = 0;
    if (!sdOk) {
        sdFail = true;
        return;
    }
    sessionNo = prefs.getInt("session", 0);
    // Siempre un número libre: nunca se pisa un archivo existente.
    do {
        sessionNo++;
        snprintf(rawName, sizeof(rawName), "/karting/s%03d_raw.csv", sessionNo);
    } while (sessionNo < 999 && SD.exists(rawName));
    prefs.putInt("session", sessionNo);
    snprintf(lapsName, sizeof(lapsName), "/karting/s%03d_laps.csv", sessionNo);
    snprintf(pathName, sizeof(pathName), "/karting/s%03d_path.csv", sessionNo);

    rawFile = SD.open(rawName, FILE_WRITE);
    lapsFile = SD.open(lapsName, FILE_WRITE);
    if (!rawFile || !lapsFile) {
        sdFail = true;
        sdClose();
        return;
    }
    const char *h1 = "t_ms,ax_mg,ay_mg,az_mg,gx_cdps,gy_cdps,gz_cdps\n";
    const char *h2 = "type,lap,lap_ms,end_ms,estimated\n";
    if (rawFile.print(h1) != strlen(h1) || lapsFile.print(h2) != strlen(h2)) sdFail = true;
    rawFile.flush();
    lapsFile.flush();
    lastFlush = millis();
}

static void logSample(uint32_t t, const kart::V3 &a, const kart::V3 &w)
{
    if (sdFail || !rawFile) return;
    int n = snprintf(logBuf + logLen, sizeof(logBuf) - logLen, "%u,%d,%d,%d,%d,%d,%d\n", (unsigned)t, (int)lroundf(a.x * 1000),
                     (int)lroundf(a.y * 1000), (int)lroundf(a.z * 1000), (int)lroundf(w.x * 100), (int)lroundf(w.y * 100),
                     (int)lroundf(w.z * 100));
    if (n > 0) logLen += n;
    uint32_t now = millis();
    if (logLen > sizeof(logBuf) - 64 || now - lastFlush >= FLUSH_MS) {
        size_t wr = rawFile.write((const uint8_t *)logBuf, logLen);
        if (wr != logLen) {
            sdFail = true;  // se deja de grabar; la medición sigue
        } else {
            rawFile.flush();
        }
        logLen = 0;
        lastFlush = now;
    }
}

static void logLap(const LapRec &l, uint32_t endMs)
{
    if (sdFail || !lapsFile) return;
    char line[80];
    int n = snprintf(line, sizeof(line), "%s,%d,%u,%u,%d\n", l.number == 0 ? "recon" : "race", l.number, (unsigned)l.ms,
                     (unsigned)endMs, l.estimated ? 1 : 0);
    if (lapsFile.write((const uint8_t *)line, n) != (size_t)n) sdFail = true;
    lapsFile.flush();
}

// Al terminar: trayectoria completa a un archivo aparte.
static void writePath()
{
    if (!sdOk || sdFail) return;
    File f = SD.open(pathName, FILE_WRITE);
    if (!f) {
        sdFail = true;
        return;
    }
    f.print("i,t_ms,x_m,y_m,speed_ms,heading_rad\n");
    char line[80];
    const kart::Track &t = est.track;
    for (int i = 0; i < t.size(); i++) {
        int n = snprintf(line, sizeof(line), "%d,%u,%.1f,%.1f,%.2f,%.3f\n", i, (unsigned)t.tm[i], t.px[i] * 0.1f, t.py[i] * 0.1f,
                         t.sp[i] * 0.25f, t.th[i] * 0.01f);
        if (f.write((const uint8_t *)line, n) != (size_t)n) {
            sdFail = true;
            break;
        }
    }
    f.flush();
    f.close();
}

// --- Sesión ------------------------------------------------------------------------------
static void addLap(int number, uint32_t ms, bool estimated, float endIdx, uint32_t endMs)
{
    LapRec l;
    l.number = number;
    l.ms = ms;
    l.estimated = estimated;
    l.endIdx = endIdx;
    laps.push_back(l);
    logLap(l, endMs);
}

static void handleEvents()
{
    for (const kart::LapEvent &e : evBuf) {
        if (e.number == 0) continue;  // el primer cruce solo fija el punto de partida
        int k = std::max(1, e.laps);
        uint32_t per = e.lapMs / k;
        for (int c = 0; c < k; c++) {
            circuits++;
            bool isRecon = recon && circuits == 1;
            if (isRecon) {
                addLap(0, per, k > 1, e.idx, e.atMs);
                beep(1400, 150);
                beep(1900, 150);
            } else {
                doneLaps++;
                addLap(doneLaps, per, k > 1, e.idx, e.atMs);
                if (doneLaps >= raceLaps && !finished) {
                    finished = true;
                    finishedAt = millis();
                    beep(2200, 350);
                    beep(2200, 350);
                    beep(2200, 700);
                } else if (!finished) {
                    beep(2000, 120);
                    beep(2000, 120);
                }
            }
        }
    }
    evBuf.clear();
}

static void beginRace()
{
    laps.clear();
    circuits = doneLaps = 0;
    finished = false;
    finishedAt = stillSince = 0;
    topSpeed = maxLatG = 0;
    stopHoldStart = 0;
    endReason = "";
}

static void stopSession(const char *reason)
{
    endReason = reason;
    sdClose();
    writePath();
    M5Cardputer.Display.setBrightness(110);
    beep(900, 500);
    screen = Screen::Done;
    dirty = true;
}

// --- IMU -------------------------------------------------------------------------------------
static void sampleImu()
{
    uint32_t now = millis();
    if (now - lastSampleMs < SAMPLE_MS) return;
    lastSampleMs = now;
    if (screen != Screen::Calibrating && screen != Screen::WaitStart && screen != Screen::Running) return;
    if (!M5.Imu.update()) return;
    float ax, ay, az, gx, gy, gz;
    M5.Imu.getAccel(&ax, &ay, &az);
    M5.Imu.getGyro(&gx, &gy, &gz);
    kart::V3 a(ax, ay, az), w(gx, gy, gz);

    if (screen != Screen::Calibrating) logSample(now, a, w);

    kart::Phase before = est.phase;
    est.update(a, w, now, evBuf);

    if (screen == Screen::Calibrating) {
        if (est.phase == kart::Phase::WaitStart) {
            sdOpenSession();
            beep(1700, 200);
            screen = Screen::WaitStart;
            M5Cardputer.Display.setBrightness(25);
        }
        dirty = true;
    } else if (screen == Screen::WaitStart && est.phase == kart::Phase::Running) {
        beginRace();
        beep(1500, 500);
        screen = Screen::Running;
        dirty = true;
    } else if (screen == Screen::Running) {
        handleEvents();
        topSpeed = std::max(topSpeed, est.speedKmh());
        maxLatG = std::max(maxLatG, fabsf(est.latG));
    }
    (void)before;
}

// --- Dibujo -----------------------------------------------------------------------------------
static void drawHud(const char *center, const char *right, uint16_t rightColor)
{
    canvas.fillRect(0, 0, SCREEN_W, HUD_H, C_HUD);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(C_ACCENT, C_HUD);
    canvas.drawString("KARTING", 4, HUD_H / 2);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_TEXT, C_HUD);
    canvas.drawString(center, SCREEN_W / 2 + 6, HUD_H / 2);
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(rightColor, C_HUD);
    canvas.drawString(right, SCREEN_W - 4, HUD_H / 2);
}

static void drawFooter(const char *text)
{
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(text, SCREEN_W / 2, SCREEN_H - 2);
}

static void drawSetup()
{
    drawHud("Configuracion", "v" APP_VERSION, C_DIM);

    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("VUELTAS DE CARRERA", SCREEN_W / 2, 26);
    canvas.setFont(&fonts::Font7);
    canvas.setTextColor(C_ACCENT, C_BG);
    canvas.drawString(String(raceLaps), SCREEN_W / 2, 62);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("<", SCREEN_W / 2 - 52, 62);
    canvas.drawString(">", SCREEN_W / 2 + 52, 62);

    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(recon ? C_GREEN : C_DIM, C_BG);
    canvas.drawString(recon ? "r  vuelta de reconocimiento: SI" : "r  vuelta de reconocimiento: NO", SCREEN_W / 2, 96);
    canvas.setTextColor(sdOk ? C_DIM : C_RED, C_BG);
    canvas.drawString(sdNote, SCREEN_W / 2, 109);
    drawFooter("; . +-1  , / +-5  r reconocim.  enter");
}

static void drawCalibrating()
{
    drawHud("Calibracion", "", C_DIM);
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font2);
    if (est.phase == kart::Phase::Calibrating) {
        canvas.setTextColor(C_ACCENT, C_BG);
        canvas.drawString("Quieto...", SCREEN_W / 2, 44);
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(est.calFailed ? C_RED : C_DIM, C_BG);
        canvas.drawString(est.calFailed ? "Se movio: empieza de nuevo" : "Midiendo gravedad y giroscopo", SCREEN_W / 2, 68);
        canvas.drawRect(30, 84, 180, 8, C_DIM);
        canvas.fillRect(31, 85, (int)(178 * est.calProgress), 6, C_ACCENT);
    } else {
        canvas.setTextColor(C_ACCENT, C_BG);
        canvas.drawString("Sentate y aprieta G0", SCREEN_W / 2, 50);
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("Con el kart parado y el aparato en el bolsillo", SCREEN_W / 2, 74);
        canvas.drawString("(o enter, para probar en la mesa)", SCREEN_W / 2, 88);
    }
    drawFooter("del  volver");
}

static void drawWaitStart()
{
    drawHud("Listo", sdFail ? "SD ERROR" : "REC SD", sdFail ? C_RED : C_GREEN);
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_GREEN, C_BG);
    canvas.drawString("Esperando el arranque", SCREEN_W / 2, 50);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(String(raceLaps) + " vueltas" + (recon ? " + reconocimiento" : ""), SCREEN_W / 2, 74);
    canvas.drawString("Arranca solo al acelerar", SCREEN_W / 2, 88);
    drawFooter("G0 calibrar de nuevo   del volver");
}

// Mejor vuelta de carrera (0 = ninguna).
static uint32_t bestLap(int *bestNo = nullptr)
{
    uint32_t best = 0;
    for (const LapRec &l : laps) {
        if (l.number == 0 || l.estimated) continue;
        if (!best || l.ms < best) {
            best = l.ms;
            if (bestNo) *bestNo = l.number;
        }
    }
    return best;
}

static void drawMiniMap(int x0, int y0, int w, int h)
{
    const kart::Track &t = est.track;
    int n = t.size();
    if (n < 4) return;
    int step = std::max(1, n / 140);
    int16_t minX = 32767, maxX = -32767, minY = 32767, maxY = -32767;
    for (int i = 0; i < n; i += step) {
        minX = std::min(minX, t.px[i]), maxX = std::max(maxX, t.px[i]);
        minY = std::min(minY, t.py[i]), maxY = std::max(maxY, t.py[i]);
    }
    float sx = (maxX - minX) > 1 ? (float)(w - 6) / (maxX - minX) : 1, sy = (maxY - minY) > 1 ? (float)(h - 6) / (maxY - minY) : 1;
    float s = std::min(sx, sy);
    auto X = [&](int i) { return x0 + 3 + (int)((t.px[i] - minX) * s); };
    auto Y = [&](int i) { return y0 + h - 3 - (int)((t.py[i] - minY) * s); };
    for (int i = step; i < n; i += step) canvas.drawLine(X(i - step), Y(i - step), X(i), Y(i), C_DIM);
    canvas.fillCircle(X(n - 1), Y(n - 1), 3, C_ACCENT);
}

static void drawRunning()
{
    uint32_t now = millis();
    char buf[24];
    bool locked = est.laps.locked() && est.laps.crossings.size() >= 2;
    uint32_t lapRef = locked ? est.laps.crossings.back().atMs : est.startedAt;

    uint32_t total = now - est.startedAt;
    fmtTime(total, buf, sizeof(buf));
    drawHud(finished ? "FINAL" : (recon && circuits == 0 ? "RECONOCIMIENTO" : "CARRERA"), buf, sdFail ? C_RED : C_TEXT);
    canvas.fillCircle(SCREEN_W - 58, HUD_H / 2, 3, sdFail ? C_RED : ((now / 500) % 2 ? C_RED : C_HUD));

    // Vuelta actual y tiempo
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(finished ? "VUELTAS" : "VUELTA", 8, 22);
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(finished ? C_GREEN : C_ACCENT, C_BG);
    canvas.drawString(String(std::min(doneLaps + (finished ? 0 : 1), raceLaps)) + "/" + String(raceLaps), 8, 32);

    fmtTime(now - lapRef, buf, sizeof(buf));
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(buf, 8, 58);
    if (!locked) {
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("aprendiendo el circuito", 8, 82);
    }

    // Última, mejor y diferencia
    int bestNo = 0;
    uint32_t best = bestLap(&bestNo);
    uint32_t last = 0;
    for (const LapRec &l : laps) {
        if (l.number > 0) last = l.ms;
    }
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);
    if (last) {
        fmtTime(last, buf, sizeof(buf));
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("ULT", 8, 94);
        canvas.setTextColor(C_TEXT, C_BG);
        canvas.drawString(buf, 32, 94);
        if (best) {
            long d = (long)last - (long)best;
            snprintf(buf, sizeof(buf), "%+.2f", d / 1000.0);
            canvas.setTextColor(d <= 0 ? C_GREEN : C_YELLOW, C_BG);
            canvas.drawString(buf, 90, 94);
        }
    }
    if (best) {
        fmtTime(best, buf, sizeof(buf));
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("MEJ", 8, 105);
        canvas.setTextColor(C_GREEN, C_BG);
        canvas.drawString(buf, 32, 105);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("V" + String(bestNo), 90, 105);
    }

    // Mapa y velocidad
    drawMiniMap(150, 20, 86, 80);
    canvas.setTextDatum(top_right);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(String((int)est.speedKmh()) + " km/h", SCREEN_W - 4, 104);

    // Cuenta regresiva para terminar
    if (M5Cardputer.BtnA.isPressed() && M5Cardputer.BtnA.pressedFor(800)) {
        uint32_t held = M5Cardputer.BtnA.lastChange() ? now - M5Cardputer.BtnA.lastChange() : 0;
        canvas.fillRect(0, SCREEN_H - 16, SCREEN_W, 16, C_RED);
        canvas.fillRect(0, SCREEN_H - 16, std::min<uint32_t>(SCREEN_W, held * SCREEN_W / STOP_BTN_MS), 16, C_ACCENT);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(C_BG, C_ACCENT);
        canvas.drawString("seguí apretando G0 para terminar", SCREEN_W / 2, SCREEN_H - 8);
    } else if (stopHoldStart) {
        uint32_t held = now - stopHoldStart;
        canvas.fillRect(0, SCREEN_H - 16, SCREEN_W, 16, C_RED);
        canvas.fillRect(0, SCREEN_H - 16, std::min<uint32_t>(SCREEN_W, held * SCREEN_W / STOP_KEY_MS), 16, C_ACCENT);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(C_BG, C_ACCENT);
        canvas.drawString("segui apretando enter para terminar", SCREEN_W / 2, SCREEN_H - 8);
    } else {
        drawFooter("G0 5 s o enter 3 s: terminar");
    }
}

// --- Resultados ----------------------------------------------------------------------------------
static int donePage;
static int lapScroll;

static void drawLapsPage()
{
    int bestNo = 0;
    uint32_t best = bestLap(&bestNo);
    char buf[24];
    canvas.setFont(&fonts::Font0);
    if (laps.empty()) {
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(C_RED, C_BG);
        canvas.drawString("No se detectaron vueltas", SCREEN_W / 2, 56);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("(hace falta repetir el circuito al menos 2 veces)", SCREEN_W / 2, 72);
        return;
    }
    const int rows = 9;
    lapScroll = constrain(lapScroll, 0, std::max(0, (int)laps.size() - rows));
    canvas.setTextDatum(top_left);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("VUELTA", 12, 19);
    canvas.drawString("TIEMPO", 80, 19);
    canvas.drawString("DIF", 150, 19);
    for (int r = 0; r < rows && lapScroll + r < (int)laps.size(); r++) {
        const LapRec &l = laps[lapScroll + r];
        int y = 30 + r * 10;
        bool isBest = l.number > 0 && !l.estimated && l.ms == best;
        if (isBest) canvas.fillRect(6, y - 1, SCREEN_W - 12, 10, C_HUD);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(l.number == 0 ? C_DIM : C_TEXT, isBest ? C_HUD : C_BG);
        canvas.drawString(l.number == 0 ? "Recon" : String(l.number) + (l.estimated ? "*" : ""), 12, y);
        fmtTime(l.ms, buf, sizeof(buf));
        canvas.setTextColor(isBest ? C_GREEN : (l.number == 0 ? C_DIM : C_TEXT), isBest ? C_HUD : C_BG);
        canvas.drawString(buf, 80, y);
        if (l.number > 0 && best && !isBest) {
            snprintf(buf, sizeof(buf), "%+.2f", ((long)l.ms - (long)best) / 1000.0);
            canvas.setTextColor(C_YELLOW, C_BG);
            canvas.drawString(buf, 150, y);
        } else if (isBest) {
            canvas.setTextColor(C_GREEN, C_HUD);
            canvas.drawString("mejor", 150, y);
        }
    }
    if ((int)laps.size() > rows) {
        canvas.setTextColor(C_DIM, C_BG);
        canvas.setTextDatum(top_right);
        canvas.drawString(String(lapScroll + 1) + "-" + String(std::min<int>(laps.size(), lapScroll + rows)) + "/" + String(laps.size()),
                          SCREEN_W - 8, 19);
    }
}

static uint16_t speedColor(float v)  // 0..25 m/s: azul -> verde -> rojo
{
    float f = constrain(v / 25.f, 0.f, 1.f);
    uint8_t r = f < 0.5f ? 0 : (uint8_t)((f - 0.5f) * 2 * 255);
    uint8_t g = f < 0.5f ? (uint8_t)(120 + f * 2 * 135) : (uint8_t)(255 - (f - 0.5f) * 2 * 175);
    uint8_t b = f < 0.5f ? (uint8_t)(255 - f * 2 * 255) : 0;
    return canvas.color565(r, g, b);
}

static void drawMapPage()
{
    std::vector<float> xs, ys;
    std::vector<uint8_t> vs;
    int bestNo = 0;
    uint32_t best = bestLap(&bestNo);
    bool closed = false;
    if (best) {
        // La vuelta más rápida, cerrada: reparte el desvío para que dibuje un lazo.
        int cross = -1;
        int count = 0;
        for (const LapRec &l : laps) {
            if (l.number == bestNo) {
                cross = count;
                break;
            }
            count++;
        }
        // laps[] incluye la vuelta de reconocimiento; el cruce que cierra la vuelta i es crossings[i+1].
        // Con cruces perdidos los índices se corren, por eso se busca por posición final.
        for (size_t i = 1; i < est.laps.crossings.size(); i++) {
            if (fabsf(est.laps.crossings[i].idx - laps[cross].endIdx) < 0.01f) {
                est.laps.closedLap((int)i - 1, xs, ys, vs);
                closed = !xs.empty();
                break;
            }
        }
    }
    if (!closed) {
        const kart::Track &t = est.track;
        int step = std::max(1, t.size() / 400);
        for (int i = 0; i < t.size(); i += step) {
            xs.push_back(t.px[i] * 0.1f);
            ys.push_back(t.py[i] * 0.1f);
            vs.push_back(t.sp[i]);
        }
    }
    if (xs.size() < 2) {
        canvas.setFont(&fonts::Font0);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("Sin trayectoria", SCREEN_W / 2, 60);
        return;
    }
    float minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
    for (size_t i = 0; i < xs.size(); i++) {
        minX = std::min(minX, xs[i]), maxX = std::max(maxX, xs[i]);
        minY = std::min(minY, ys[i]), maxY = std::max(maxY, ys[i]);
    }
    const int bx = 10, by = 22, bw = SCREEN_W - 20, bh = 94;
    float s = std::min((bw - 4) / std::max(1.f, maxX - minX), (bh - 4) / std::max(1.f, maxY - minY));
    int ox = bx + (bw - (int)((maxX - minX) * s)) / 2, oy = by + (bh - (int)((maxY - minY) * s)) / 2;
    auto X = [&](size_t i) { return ox + (int)((xs[i] - minX) * s); };
    auto Y = [&](size_t i) { return oy + (int)((maxY - ys[i]) * s); };
    for (size_t i = 1; i < xs.size(); i++) {
        uint16_t c = speedColor(vs[i] * 0.25f);
        canvas.drawLine(X(i - 1), Y(i - 1), X(i), Y(i), c);
        canvas.drawLine(X(i - 1) + 1, Y(i - 1), X(i) + 1, Y(i), c);
    }
    canvas.fillCircle(X(0), Y(0), 3, C_TEXT);
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(closed ? "Mejor vuelta (V" + String(bestNo) + ")" : "Recorrido completo", 10, 17);
    canvas.setTextDatum(top_right);
    float len = (maxX - minX);
    (void)len;
    canvas.drawString("lento", SCREEN_W - 52, 17);
    for (int i = 0; i < 40; i++) canvas.drawFastVLine(SCREEN_W - 48 + i, 18, 6, speedColor(i / 40.f * 25.f));
    canvas.setTextDatum(top_left);
    canvas.drawString("rapido", SCREEN_W - 7 - 30 + 30, 17);
}

static void drawInfoPage()
{
    char buf[48];
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);
    int y = 20;
    auto line = [&](const char *k, const String &v, uint16_t c) {
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString(k, 10, y);
        canvas.setTextColor(c, C_BG);
        canvas.drawString(v, 100, y);
        y += 12;
    };
    line("Fin", endReason, C_TEXT);
    snprintf(buf, sizeof(buf), "%.2f km", est.s / 1000.f);
    line("Distancia", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%d km/h", (int)topSpeed);
    line("Max velocidad", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%.2f g", maxLatG);
    line("Max lateral", buf, C_TEXT);
    if (est.laps.locked()) {
        snprintf(buf, sizeof(buf), "%d m", (int)(est.laps.lapLength() * kart::DS));
        line("Largo circuito", buf, C_TEXT);
    }
    line("Vueltas", String(doneLaps) + "/" + String(raceLaps), doneLaps >= raceLaps ? C_GREEN : C_YELLOW);
    line("SD", sdFail ? "ERROR de escritura" : "s" + String(sessionNo) + " guardada", sdFail ? C_RED : C_GREEN);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(rawName, 10, y + 2);
}

static void drawDone()
{
    static const char *TITLES[3] = {"Vueltas", "Trazado", "Datos"};
    drawHud(TITLES[donePage], sdFail ? "SD ERROR" : "SD OK", sdFail ? C_RED : C_GREEN);
    switch (donePage) {
        case 0: drawLapsPage(); break;
        case 1: drawMapPage(); break;
        default: drawInfoPage(); break;
    }
    drawFooter("tab pagina   ; . mover   enter nueva sesion");
}

static void render()
{
    canvas.fillScreen(C_BG);
    switch (screen) {
        case Screen::Setup: drawSetup(); break;
        case Screen::Calibrating: drawCalibrating(); break;
        case Screen::WaitStart: drawWaitStart(); break;
        case Screen::Running: drawRunning(); break;
        case Screen::Done: drawDone(); break;
    }
    canvas.pushSprite(0, 0);
}

// --- Entrada ----------------------------------------------------------------------------------------
static bool hasChar(const Keyboard_Class::KeysState &ks, const char *chars)
{
    for (char c : ks.word) {
        if (strchr(chars, c)) return true;
    }
    return false;
}

static void startCalibration()
{
    est.reset();
    est.startCalibration();
    screen = Screen::Calibrating;
    dirty = true;
}

static void handleKeyPress(const Keyboard_Class::KeysState &ks)
{
    switch (screen) {
        case Screen::Setup:
            if (hasChar(ks, ";") && raceLaps < 99) raceLaps++;
            if (hasChar(ks, ".") && raceLaps > 1) raceLaps--;
            if (hasChar(ks, "/") ) raceLaps = std::min(99, raceLaps + 5);
            if (hasChar(ks, ",")) raceLaps = std::max(1, raceLaps - 5);
            if (hasChar(ks, "rR")) recon = !recon;
            if (ks.enter) {
                prefs.putInt("laps", raceLaps);
                prefs.putBool("recon", recon);
                est.reset();
                screen = Screen::Calibrating;
                sdOk = sdOk || sdInit();
            }
            break;
        case Screen::Calibrating:
            if (ks.del) screen = Screen::Setup;
            if (ks.enter && est.phase != kart::Phase::Calibrating) est.startCalibration();
            break;
        case Screen::WaitStart:
            if (ks.del) {
                sdClose();
                M5Cardputer.Display.setBrightness(110);
                est.reset();
                screen = Screen::Setup;
            }
            break;
        case Screen::Running:
            break;  // en el bolsillo las teclas se pueden apretar solas: solo cuenta mantener enter
        case Screen::Done:
            if (hasChar(ks, "\t") || ks.tab) donePage = (donePage + 1) % 3;
            if (hasChar(ks, ";")) lapScroll--;
            if (hasChar(ks, ".")) lapScroll++;
            if (ks.enter) {
                est.reset();
                laps.clear();
                screen = Screen::Setup;
            }
            break;
    }
    dirty = true;
}

static void handleInput()
{
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        handleKeyPress(M5Cardputer.Keyboard.keysState());
    }

    // G0
    if (M5Cardputer.BtnA.wasPressed()) {
        if (screen == Screen::Calibrating && est.phase != kart::Phase::Calibrating) {
            est.startCalibration();
        } else if (screen == Screen::WaitStart) {
            sdClose();
            est.reset();
            est.startCalibration();
            screen = Screen::Calibrating;
        }
        dirty = true;
    }

    if (screen == Screen::Running) {
        // Terminar: G0 5 s, o enter 3 s. Una parada brusca no termina nada.
        if (M5Cardputer.BtnA.isPressed() && M5Cardputer.BtnA.pressedFor(STOP_BTN_MS)) {
            stopSession("manual (G0)");
            return;
        }
        bool enterDown = M5Cardputer.Keyboard.isPressed() && M5Cardputer.Keyboard.keysState().enter;
        if (enterDown) {
            if (!stopHoldStart) stopHoldStart = millis();
            if (millis() - stopHoldStart >= STOP_KEY_MS) {
                stopSession("manual (teclado)");
                return;
            }
        } else {
            stopHoldStart = 0;
        }
    }
}

static void tick()
{
    uint32_t now = millis();
    beepTick();

    if (screen == Screen::Running) {
        // Carrera terminada y el kart quieto un buen rato: cierra solo.
        if (finished) {
            if (est.moving) stillSince = 0;
            else if (!stillSince) stillSince = now;
            if (stillSince && now - stillSince > AUTO_CLOSE_MS) {
                stopSession("fin de carrera");
                return;
            }
        }
        if (now - lastBattCheck > 10000) {
            lastBattCheck = now;
            int lvl = M5Cardputer.Power.getBatteryLevel();
            if (lvl >= 0 && lvl <= 3) {
                stopSession("bateria baja");
                return;
            }
        }
        static uint32_t lastDraw;
        if (now - lastDraw >= 250 || M5Cardputer.BtnA.isPressed()) {
            lastDraw = now;
            dirty = true;
        }
    } else if (screen == Screen::Calibrating) {
        dirty = true;
    }
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(110);
    M5Cardputer.Speaker.setVolume(255);

    canvas.setColorDepth(16);
    canvas.createSprite(SCREEN_W, SCREEN_H);

    C_BG = canvas.color565(12, 12, 16);
    C_HUD = canvas.color565(34, 30, 30);
    C_TEXT = canvas.color565(225, 225, 230);
    C_DIM = canvas.color565(120, 120, 130);
    C_ACCENT = canvas.color565(255, 110, 40);
    C_RED = canvas.color565(221, 29, 33);
    C_GREEN = canvas.color565(80, 210, 110);
    C_YELLOW = canvas.color565(255, 214, 0);

    prefs.begin("karting", false);
    raceLaps = constrain(prefs.getInt("laps", 10), 1, 99);
    recon = prefs.getBool("recon", true);

    sdOk = sdInit();
    if (!M5.Imu.isEnabled()) {
        sdNote = "No se encontro el IMU";
        sdOk = false;
    }
    render();
}

void loop()
{
    handleInput();
    sampleImu();
    tick();
    if (dirty && screen != Screen::Calibrating) {
        render();
        dirty = false;
    } else if (screen == Screen::Calibrating) {
        static uint32_t lastCal;
        if (millis() - lastCal > 100) {
            lastCal = millis();
            render();
        }
        dirty = false;
    }
    delay(1);
}
