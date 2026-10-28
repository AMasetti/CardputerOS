// TV-B-Gone para M5Stack Cardputer-Adv
//
// Dispara por el LED infrarrojo (G44) los códigos de encendido/apagado de
// cientos de modelos de televisores, uno detrás de otro, como el llavero
// TV-B-Gone original de Mitch Altman. Apuntá el extremo superior del
// Cardputer (donde está el LED IR) a la tele.
//
//   enter  space   disparar / frenar
//   ;  .           región: América (NA), Europa (EU) o ambas
//   l              repetir la secuencia sin parar
//   del  `         frenar
//
// La región y la repetición se guardan en la flash (NVS).

#include <M5Cardputer.h>
#include <Preferences.h>
#include <esp_timer.h>

#include "world_ir_codes.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static constexpr int IR_PIN = 44;
static constexpr int IR_CH = 0;
static constexpr int IR_RES = 8;
static constexpr uint32_t DUTY = 85;          // ~33 %, como el original
static constexpr uint32_t GAP_MS = 205;       // pausa entre códigos

enum class Region : uint8_t { NA, EU, Both };
static const char *REGION_NAME[] = {"América", "Europa", "NA + EU"};
static const char *REGION_HINT[] = {"NA: América, Asia y resto", "EU: Europa, Oceanía, M. Oriente", "Todos los códigos"};

static Preferences prefs;
static Region region = Region::NA;
static bool loopAll = false;

static bool sending = false;
static bool finished = false;
static int idx = 0;               // próximo código a mandar
static int sentCount = 0;
static int rounds = 0;
static uint32_t nextAt = 0;
static uint32_t lastFreq = 0;
static char lastName[8] = "";

static M5Canvas spr(&M5Cardputer.Display);

static const uint16_t C_BG = lgfx::color565(12, 12, 18);
static const uint16_t C_PANEL = lgfx::color565(30, 30, 42);
static const uint16_t C_DIM = lgfx::color565(110, 110, 130);
static const uint16_t C_TEXT = lgfx::color565(230, 230, 240);
static const uint16_t C_RED = lgfx::color565(255, 60, 60);
static const uint16_t C_IR = lgfx::color565(200, 60, 255);
static const uint16_t C_GREEN = lgfx::color565(60, 220, 110);

// --- Base de códigos ----------------------------------------------------------
static int total()
{
    switch (region) {
    case Region::NA: return num_NAcodes;
    case Region::EU: return num_EUcodes;
    default: return num_NAcodes + num_EUcodes;
    }
}

static const IrCode *codeAt(int i, char *name)
{
    bool eu = region == Region::EU || (region == Region::Both && i >= num_NAcodes);
    if (region == Region::Both && eu) i -= num_NAcodes;
    snprintf(name, 8, "%s%03d", eu ? "EU" : "NA", i);
    return eu ? EUpowerCodes[i] : NApowerCodes[i];
}

// --- Emisión ---------------------------------------------------------------------
static inline void waitUntil(int64_t t)
{
    while (esp_timer_get_time() < t) {
    }
}

static void irOn(bool carrier) { ledcWrite(IR_CH, carrier ? DUTY : (1 << IR_RES) - 1); }
static void irOff() { ledcWrite(IR_CH, 0); }

// Lee los índices empaquetados de a `count` bits, del bit más alto al más bajo.
struct BitReader {
    const uint8_t *p;
    uint8_t bits = 0, left = 0;
    uint8_t read(uint8_t count)
    {
        uint8_t v = 0;
        for (uint8_t i = 0; i < count; ++i) {
            if (!left) bits = *p++, left = 8;
            --left;
            v |= ((bits >> left) & 1) << (count - 1 - i);
        }
        return v;
    }
};

static void sendCode(const IrCode *c)
{
    bool carrier = c->freq != 0;
    if (carrier) ledcSetup(IR_CH, c->freq, IR_RES);
    BitReader br;
    br.p = c->codes;
    // Tiempos absolutos: así los retrasos de una transición no se acumulan.
    int64_t t = esp_timer_get_time();
    for (uint8_t k = 0; k < c->numpairs; ++k) {
        uint8_t ti = br.read(c->bitcompression);
        uint32_t on = c->times[ti * 2] * 10;
        uint32_t off = c->times[ti * 2 + 1] * 10;
        irOn(carrier);
        t += on;
        waitUntil(t);
        irOff();
        t += off;
        waitUntil(t);
    }
    irOff();
}

// --- Dibujo --------------------------------------------------------------------------
static void drawTv(int x, int y, bool hit)
{
    // Patas, gabinete y pantalla.
    spr.fillTriangle(x + 22, y + 52, x + 30, y + 46, x + 30, y + 52, C_DIM);
    spr.fillTriangle(x + 58, y + 52, x + 50, y + 46, x + 50, y + 52, C_DIM);
    spr.fillRoundRect(x, y, 80, 48, 6, C_PANEL);
    spr.fillRoundRect(x + 5, y + 5, 70, 38, 3, hit ? lgfx::color565(5, 5, 8) : lgfx::color565(40, 80, 140));
    if (!hit) { // reflejo de una pantalla prendida
        spr.drawLine(x + 52, y + 9, x + 68, y + 9, lgfx::color565(120, 170, 230));
        spr.drawLine(x + 60, y + 13, x + 68, y + 13, lgfx::color565(120, 170, 230));
    }
    // Símbolo de encendido.
    int cx = x + 40, cy = y + 25;
    uint16_t pc = hit ? C_RED : C_TEXT;
    spr.drawArc(cx, cy, 11, 9, -50, 230, pc);
    spr.fillRect(cx - 1, cy - 14, 3, 13, pc);
}

static void drawWaves(int x, int y, uint32_t phase)
{
    // Tres arcos que avanzan hacia la tele.
    for (int i = 0; i < 4; ++i) {
        int r = 10 + ((phase / 40 + i * 14) % 56);
        uint8_t a = 255 - r * 3;
        uint16_t col = lgfx::color565(a * 200 / 255, a * 60 / 255, a);
        spr.drawArc(x, y, r, r - 2, -35, 35, col);
    }
    spr.fillCircle(x, y, 4, C_IR);
}

static void render()
{
    uint32_t now = millis();
    spr.fillSprite(C_BG);

    // Encabezado.
    spr.fillRect(0, 0, 240, 16, C_PANEL);
    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextDatum(textdatum_t::middle_left);
    spr.setTextColor(C_TEXT);
    spr.drawString("TV-B-Gone", 4, 8);
    spr.setTextDatum(textdatum_t::middle_right);
    spr.setTextColor(loopAll ? C_IR : C_DIM);
    spr.drawString(loopAll ? "repetir" : "una vez", 236, 8);
    spr.setTextColor(C_TEXT);
    spr.setTextDatum(textdatum_t::middle_center);
    spr.drawString(REGION_NAME[(int)region], 130, 8);

    // Escena.
    bool hitShown = finished || (sending && (now / 300) % 2);
    drawTv(140, 24, hitShown);
    if (sending) drawWaves(36, 48, now);
    else spr.fillCircle(36, 48, 4, C_DIM);
    spr.fillRoundRect(16, 40, 20, 16, 3, C_PANEL); // el Cardputer
    spr.fillRect(24, 44, 8, 8, C_BG);

    // Estado.
    spr.setFont(&fonts::lgfxJapanGothicP_16);
    spr.setTextDatum(textdatum_t::middle_center);
    char buf[48];
    int n = total();
    if (sending) {
        spr.setTextColor(C_IR);
        snprintf(buf, sizeof(buf), "Disparando %s", lastName);
    } else if (finished) {
        spr.setTextColor(C_GREEN);
        snprintf(buf, sizeof(buf), "Listo: %d códigos", sentCount);
    } else {
        spr.setTextColor(C_TEXT);
        snprintf(buf, sizeof(buf), "Apuntá a la tele");
    }
    spr.drawString(buf, 120, 88);

    // Barra de progreso.
    int done = sending || finished ? (finished ? n : idx) : 0;
    spr.fillRoundRect(10, 100, 220, 8, 4, C_PANEL);
    if (n && done) spr.fillRoundRect(10, 100, std::max(8, 220 * done / n), 8, 4, finished ? C_GREEN : C_IR);

    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextColor(C_DIM);
    spr.setTextDatum(textdatum_t::middle_left);
    snprintf(buf, sizeof(buf), "%d/%d", done, n);
    spr.drawString(buf, 10, 116);
    spr.setTextDatum(textdatum_t::middle_right);
    if (sending) {
        if (lastFreq) snprintf(buf, sizeof(buf), "%.1f kHz", lastFreq / 1000.0f);
        else snprintf(buf, sizeof(buf), "sin portadora");
        if (rounds) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " · vuelta %d", rounds + 1);
    } else {
        snprintf(buf, sizeof(buf), "%s", REGION_HINT[(int)region]);
    }
    spr.drawString(buf, 230, 116);

    spr.setTextDatum(textdatum_t::middle_center);
    spr.drawString(sending ? "enter / del: frenar" : "enter: disparar   ; . región   l: repetir", 120, 129);
    spr.pushSprite(0, 0);
}

// --- Control ---------------------------------------------------------------------------
static void savePrefs()
{
    prefs.putUChar("region", (uint8_t)region);
    prefs.putBool("loop", loopAll);
}

static void start()
{
    sending = true;
    finished = false;
    idx = 0;
    sentCount = 0;
    rounds = 0;
    nextAt = millis();
}

static void stop()
{
    sending = false;
    irOff();
}

static void handleKeys()
{
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto st = M5Cardputer.Keyboard.keysState();
    if (st.enter || st.space) {
        if (sending) stop();
        else start();
        return;
    }
    if (st.del) {
        stop();
        return;
    }
    for (char c : st.word) {
        switch (c) {
        case '`':
            stop();
            break;
        case ';':
        case '.':
            if (!sending) {
                region = (Region)(((int)region + (c == '.' ? 1 : 2)) % 3);
                finished = false;
                savePrefs();
            }
            break;
        case 'l':
        case 'L':
            loopAll = !loopAll;
            savePrefs();
            break;
        }
    }
}

static void step()
{
    if (!sending || (int32_t)(millis() - nextAt) < 0) return;
    if (idx >= total()) {
        if (!loopAll) {
            sending = false;
            finished = true;
            return;
        }
        idx = 0;
        ++rounds;
    }
    const IrCode *c = codeAt(idx, lastName);
    lastFreq = c->freq;
    sendCode(c);
    ++idx;
    ++sentCount;
    nextAt = millis() + GAP_MS;
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    prefs.begin("tvbgone", false);
    region = (Region)std::min<uint8_t>(prefs.getUChar("region", 0), 2);
    loopAll = prefs.getBool("loop", false);

    ledcSetup(IR_CH, 38000, IR_RES);
    ledcAttachPin(IR_PIN, IR_CH);
    irOff();

    spr.setColorDepth(16);
    spr.createSprite(240, 135);
}

void loop()
{
    M5Cardputer.update();
    handleKeys();
    step();
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw >= 33) {
        lastDraw = millis();
        render();
    }
    delay(1);
}
