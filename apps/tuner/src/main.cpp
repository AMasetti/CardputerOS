// Afinador para M5Stack Cardputer-Adv
//
// Escucha con el micrófono interno, detecta la frecuencia con YIN y muestra
// la nota en grande con un medidor de cents. Además del modo cromático trae
// afinaciones de guitarra, bajo y ukelele: elige sola la cuerda más cercana
// (o la que fijes a mano) y marca las que ya quedaron afinadas.
//
//   ;  .         afinación anterior / siguiente
//   ,  /         cuerda anterior / siguiente (fija la cuerda)
//   enter        volver a elegir la cuerda automáticamente
//   space        tono de referencia de la cuerda (o de la última nota)
//   -  =         La de referencia (A4) entre 430 y 450 Hz
//   n            notación C D E / Do Re Mi
//
// La afinación, el A4 y la notación se guardan en la flash (NVS).

#include <M5Cardputer.h>
#include <Preferences.h>
#include <math.h>
#include <algorithm>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// --- Audio y detección ------------------------------------------------------
static constexpr uint32_t RATE = 16000;
static constexpr size_t CHUNK = 512;             // 32 ms por bloque del micrófono
static constexpr size_t FRAME = 2048;            // ventana de análisis (128 ms)
static constexpr size_t YIN_W = 1024;            // ventana de integración de YIN
static constexpr int TAU_MIN = RATE / 1400;      // nota más aguda: ~1400 Hz
static constexpr int TAU_MAX = RATE / 38;        // más grave: ~38 Hz (Mi del bajo)
static constexpr float YIN_THRESHOLD = 0.15f;
static constexpr float GATE_RMS = 40.0f;         // por debajo, silencio
static constexpr uint32_t HOLD_MS = 1500;        // cuánto queda la última nota
static constexpr float IN_TUNE_CENTS = 5.0f;
static constexpr uint32_t TUNED_MS = 600;        // afinada sostenida para marcar la cuerda

static int16_t chunks[3][CHUNK];
static int16_t ring[FRAME];
static size_t ringPos = 0;
static uint32_t recCount = 0;
static float frameBuf[FRAME];
static float diffBuf[TAU_MAX + 2]; // diferencia normalizada (YIN)
static float rawBuf[TAU_MAX + 2];  // diferencia sin normalizar, para interpolar

// --- Afinaciones ----------------------------------------------------------------
struct Tuning {
    const char *name;
    uint8_t n;
    uint8_t midi[6]; // de la cuerda más grave (izquierda) a la más aguda
};

static const Tuning TUNINGS[] = {
    {"Cromático", 0, {}},
    {"Guitarra · Estándar", 6, {40, 45, 50, 55, 59, 64}},
    {"Guitarra · Drop D", 6, {38, 45, 50, 55, 59, 64}},
    {"Guitarra · ½ tono abajo", 6, {39, 44, 49, 54, 58, 63}},
    {"Guitarra · Drop C", 6, {36, 43, 48, 53, 57, 62}},
    {"Guitarra · DADGAD", 6, {38, 45, 50, 55, 57, 62}},
    {"Guitarra · Open G", 6, {38, 43, 50, 55, 59, 62}},
    {"Guitarra · Open D", 6, {38, 45, 50, 54, 57, 62}},
    {"Bajo · 4 cuerdas", 4, {28, 33, 38, 43}},
    {"Ukelele · GCEA", 4, {67, 60, 64, 69}},
};
static constexpr int NUM_TUNINGS = sizeof(TUNINGS) / sizeof(TUNINGS[0]);

static const char *NAMES_ABC[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *NAMES_DOREMI[12] = {"Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"};

// --- Estado ---------------------------------------------------------------------
static Preferences prefs;
static int tuningIdx = 1;
static int a4 = 440;
static bool doremi = false;

static int manualString = -1; // -1: automático
static int autoString = 0;
static uint8_t tunedMask = 0;
static uint32_t inTuneSince = 0;

static bool haveNote = false;
static uint32_t lastHeard = 0;
static float midiSmooth = 0;   // nota detectada (en semitonos MIDI, con decimales)
static float hist[5];
static int histN = 0;
static float lastFreq = 0;
static float level = 0;        // nivel de entrada 0..1 para el vúmetro
static float needle = 0;       // cents mostrados (suavizados para la aguja)

static M5Canvas spr(&M5Cardputer.Display);

// --- Colores --------------------------------------------------------------------
static const uint16_t C_BG = lgfx::color565(10, 14, 22);
static const uint16_t C_PANEL = lgfx::color565(24, 30, 42);
static const uint16_t C_DIM = lgfx::color565(90, 100, 120);
static const uint16_t C_TEXT = lgfx::color565(225, 230, 240);
static const uint16_t C_GREEN = lgfx::color565(60, 230, 120);
static const uint16_t C_GREEN_DK = lgfx::color565(20, 70, 40);
static const uint16_t C_YELLOW = lgfx::color565(255, 200, 40);
static const uint16_t C_RED = lgfx::color565(255, 80, 70);
static const uint16_t C_ACCENT = lgfx::color565(80, 160, 255);

// --- Utilidades -------------------------------------------------------------------
static const Tuning &tuning() { return TUNINGS[tuningIdx]; }
static bool chromatic() { return tuning().n == 0; }
static const char *noteName(int midi) { return (doremi ? NAMES_DOREMI : NAMES_ABC)[((midi % 12) + 12) % 12]; }
static int octave(int midi) { return midi / 12 - 1; }
static float midiToFreq(float m) { return a4 * powf(2.0f, (m - 69.0f) / 12.0f); }
static float freqToMidi(float f) { return 69.0f + 12.0f * log2f(f / a4); }

static uint16_t centsColor(float c)
{
    float a = fabsf(c);
    if (a <= IN_TUNE_CENTS) return C_GREEN;
    if (a <= 15) return C_YELLOW;
    return C_RED;
}

static void savePrefs()
{
    prefs.putUChar("tuning", tuningIdx);
    prefs.putUShort("a4", a4);
    prefs.putBool("doremi", doremi);
}

// --- Micrófono --------------------------------------------------------------------
// Mic.record() encola hasta dos bloques y se bloquea cuando no hay lugar. Con
// tres buffers en rueda, cuando vuelve el pedido i ya terminó el bloque i-2.
static bool readChunk(const int16_t *&out)
{
    int16_t *buf = chunks[recCount % 3];
    if (!M5Cardputer.Mic.record(buf, CHUNK, RATE)) return false;
    ++recCount;
    if (recCount < 3) return false;
    out = chunks[(recCount - 3) % 3];
    return true;
}

static void startMic()
{
    M5Cardputer.Speaker.end();
    auto cfg = M5Cardputer.Mic.config();
    cfg.sample_rate = RATE;
    M5Cardputer.Mic.config(cfg);
    M5Cardputer.Mic.begin();
    recCount = 0;
}

// --- YIN ----------------------------------------------------------------------------
// Devuelve la frecuencia en Hz o 0 si no hay una nota clara.
static float detectPitch(const int16_t *samples, float &rms)
{
    double mean = 0;
    for (size_t i = 0; i < FRAME; ++i) mean += samples[(ringPos + i) % FRAME];
    mean /= FRAME;
    double energy = 0;
    for (size_t i = 0; i < FRAME; ++i) {
        float v = samples[(ringPos + i) % FRAME] - mean;
        frameBuf[i] = v;
        energy += v * v;
    }
    rms = sqrtf(energy / FRAME);
    if (rms < GATE_RMS) return 0;

    // Diferencia y media acumulada normalizada.
    diffBuf[0] = 1;
    float running = 0;
    int tau = -1;
    for (int t = 1; t <= TAU_MAX; ++t) {
        const float *a = frameBuf;
        const float *b = frameBuf + t;
        float d = 0;
        for (size_t j = 0; j < YIN_W; ++j) {
            float x = a[j] - b[j];
            d += x * x;
        }
        rawBuf[t] = d;
        running += d;
        diffBuf[t] = running > 0 ? d * t / running : 1;
        // Primer mínimo por debajo del umbral: así no salta a una octava abajo.
        if (tau < 0 && t > TAU_MIN + 1 && diffBuf[t - 1] < YIN_THRESHOLD && diffBuf[t - 1] <= diffBuf[t]) {
            tau = t - 1;
            break;
        }
    }
    if (tau < 0) return 0;

    // Interpolación parabólica alrededor del mínimo, sobre la diferencia cruda
    // (la normalizada corre el mínimo y desafina las notas agudas).
    float s0 = rawBuf[tau - 1], s1 = rawBuf[tau], s2 = rawBuf[tau + 1];
    float den = s0 - 2 * s1 + s2;
    float better = tau + (fabsf(den) > 1e-9f ? 0.5f * (s0 - s2) / den : 0);
    return RATE / better;
}

// --- Seguimiento de la nota --------------------------------------------------------
static int targetString()
{
    if (chromatic()) return -1;
    return manualString >= 0 ? manualString : autoString;
}

static float targetMidi()
{
    int s = targetString();
    if (s >= 0) return tuning().midi[s];
    return roundf(midiSmooth);
}

static void onPitch(float freq)
{
    float m = freqToMidi(freq);
    uint32_t now = millis();

    // Mediana de las últimas 5 lecturas contra saltos sueltos.
    if (histN && fabsf(m - hist[(histN - 1) % 5]) > 1.0f) histN = 0;
    hist[histN % 5] = m;
    ++histN;
    int n = histN < 5 ? histN : 5;
    float s[5];
    memcpy(s, hist, sizeof(s));
    std::sort(s, s + n);
    float med = s[n / 2];

    if (!haveNote || now - lastHeard > 300 || fabsf(med - midiSmooth) > 0.6f) midiSmooth = med;
    else midiSmooth += 0.35f * (med - midiSmooth);
    haveNote = true;
    lastHeard = now;
    lastFreq = freq;

    // Cuerda automática: la más cercana, con un poco de histéresis.
    if (!chromatic()) {
        const Tuning &t = tuning();
        int best = 0;
        for (int i = 1; i < t.n; ++i)
            if (fabsf(midiSmooth - t.midi[i]) < fabsf(midiSmooth - t.midi[best])) best = i;
        if (best != autoString && fabsf(midiSmooth - t.midi[best]) + 0.3f < fabsf(midiSmooth - t.midi[autoString]))
            autoString = best;
    }

    float cents = (midiSmooth - targetMidi()) * 100;
    if (fabsf(cents) <= IN_TUNE_CENTS) {
        if (!inTuneSince) inTuneSince = now;
        int ts = targetString();
        if (ts >= 0 && now - inTuneSince >= TUNED_MS) tunedMask |= 1 << ts;
    } else {
        inTuneSince = 0;
    }
}

// --- Dibujo -----------------------------------------------------------------------------
static void drawNote(int midi, int cx, int cy, uint16_t color)
{
    const char *full = noteName(midi);
    char base[5];
    size_t len = strcspn(full, "#");
    memcpy(base, full, len);
    base[len] = 0;
    bool sharp = full[len] == '#';

    spr.setTextDatum(textdatum_t::middle_left);
    spr.setFont(len > 1 ? &fonts::DejaVu56 : &fonts::DejaVu72);
    int wBase = spr.textWidth(base);
    spr.setFont(&fonts::DejaVu24);
    int wSide = std::max<int>(spr.textWidth("#"), spr.textWidth("8")) + 2;
    int x = cx - (wBase + wSide) / 2;

    spr.setTextColor(color);
    spr.setFont(len > 1 ? &fonts::DejaVu56 : &fonts::DejaVu72);
    spr.drawString(base, x, cy);
    spr.setFont(&fonts::DejaVu24);
    if (sharp) spr.drawString("#", x + wBase + 2, cy - 16);
    spr.setTextColor(color == C_DIM ? C_DIM : C_TEXT);
    spr.drawString(String(octave(midi)), x + wBase + 2, cy + 16);
}

static void drawMeter(float cents, bool active, uint16_t color)
{
    const int x0 = 20, x1 = 220, cx = 120, y = 92;
    spr.fillRoundRect(x0, y - 4, x1 - x0, 9, 4, C_PANEL);
    spr.fillRect(cx - IN_TUNE_CENTS * 2, y - 4, IN_TUNE_CENTS * 4 + 1, 9, C_GREEN_DK);
    for (int c = -50; c <= 50; c += 10) {
        int x = cx + c * 2;
        int h = c == 0 ? 8 : (c % 50 == 0 ? 5 : 3);
        spr.drawFastVLine(x, y - 6 - h, h, c == 0 ? C_TEXT : C_DIM);
    }
    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextColor(C_DIM);
    spr.setTextDatum(textdatum_t::middle_center);
    spr.drawString("♭", x0 - 10, y);
    spr.drawString("♯", x1 + 10, y);
    if (!active) return;

    float c = std::max(-50.0f, std::min(50.0f, cents));
    int x = cx + (int)lroundf(c * 2);
    if (x != cx) spr.fillRect(std::min(x, cx), y - 2, abs(x - cx), 5, color);
    spr.fillTriangle(x - 5, y - 13, x + 5, y - 13, x, y - 5, color);
    spr.fillRect(x - 1, y - 5, 3, 11, color);
}

static void drawHeader()
{
    spr.fillRect(0, 0, 240, 16, C_PANEL);
    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextDatum(textdatum_t::middle_left);
    spr.setTextColor(C_TEXT);
    spr.drawString(tuning().name, 4, 8);

    spr.setTextDatum(textdatum_t::middle_right);
    spr.setTextColor(a4 == 440 ? C_DIM : C_YELLOW);
    char buf[16];
    snprintf(buf, sizeof(buf), "A4 %d", a4);
    spr.drawString(buf, 236, 8);

    // Vúmetro del micrófono.
    int w = spr.textWidth(buf);
    int lx = 236 - w - 26;
    spr.fillRect(lx, 5, 20, 6, C_BG);
    spr.fillRect(lx, 5, (int)(20 * level), 6, level > 0.85f ? C_RED : C_ACCENT);
}

static void drawStrings(int heard)
{
    const int y = 118, h = 16;
    if (chromatic()) {
        // Las 12 notas, con la que suena resaltada.
        for (int i = 0; i < 12; ++i) {
            int x = i * 20;
            bool on = heard >= 0 && heard % 12 == i;
            if (on) spr.fillRoundRect(x + 1, y, 18, h, 3, C_ACCENT);
            spr.setFont(&fonts::lgfxJapanGothicP_12);
            spr.setTextDatum(textdatum_t::middle_center);
            spr.setTextColor(on ? C_BG : (strchr(NAMES_ABC[i], '#') ? C_DIM : C_TEXT));
            const char *n = (doremi ? NAMES_DOREMI : NAMES_ABC)[i];
            if (doremi && strchr(n, '#')) n = "#";
            spr.drawString(n, x + 10, y + h / 2);
        }
        return;
    }
    const Tuning &t = tuning();
    int target = targetString();
    int cw = 240 / t.n;
    for (int i = 0; i < t.n; ++i) {
        int x = i * cw;
        bool sel = i == target;
        bool done = tunedMask & (1 << i);
        uint16_t bg = sel ? (done ? C_GREEN : C_ACCENT) : (done ? C_GREEN_DK : C_PANEL);
        spr.fillRoundRect(x + 2, y, cw - 4, h, 4, bg);
        if (sel && manualString >= 0) spr.drawRoundRect(x + 1, y - 1, cw - 2, h + 2, 5, C_TEXT);
        char label[10];
        snprintf(label, sizeof(label), "%s%d", noteName(t.midi[i]), octave(t.midi[i]));
        spr.setFont(&fonts::lgfxJapanGothicP_12);
        spr.setTextDatum(textdatum_t::middle_center);
        spr.setTextColor(sel ? C_BG : (done ? C_GREEN : C_TEXT));
        spr.drawString(label, x + cw / 2, y + h / 2);
    }
}

static void render()
{
    uint32_t now = millis();
    bool live = haveNote && now - lastHeard < 250;
    bool shown = haveNote && now - lastHeard < HOLD_MS;
    float target = targetMidi();
    float cents = (midiSmooth - target) * 100;
    if (live) needle += 0.5f * (cents - needle);

    spr.fillSprite(C_BG);
    drawHeader();

    uint16_t color = shown ? (live ? centsColor(cents) : C_DIM) : C_DIM;
    bool tuned = live && inTuneSince && now - inTuneSince >= TUNED_MS / 2;
    if (tuned) spr.drawRoundRect(60, 21, 120, 58, 10, C_GREEN);

    if (shown) {
        drawNote((int)target, 120, 50, color);

        // Pistas a los costados del nombre de la nota.
        spr.setFont(&fonts::lgfxJapanGothicP_16);
        spr.setTextDatum(textdatum_t::middle_center);
        if (live && cents < -IN_TUNE_CENTS) {
            spr.setTextColor(color);
            spr.drawString("▲", 30, 42);
            spr.setFont(&fonts::lgfxJapanGothicP_12);
            spr.drawString("subí", 30, 60);
        } else if (live && cents > IN_TUNE_CENTS) {
            spr.setTextColor(color);
            spr.drawString("▼", 210, 42);
            spr.setFont(&fonts::lgfxJapanGothicP_12);
            spr.drawString("bajá", 210, 60);
        }

        // Si estás lejos de la cuerda elegida, qué nota se escucha de verdad.
        int heardNote = (int)lroundf(midiSmooth);
        if (!chromatic() && heardNote != (int)target) {
            char buf[24];
            snprintf(buf, sizeof(buf), "suena %s%d", noteName(heardNote), octave(heardNote));
            spr.setFont(&fonts::lgfxJapanGothicP_12);
            spr.setTextColor(C_DIM);
            spr.setTextDatum(textdatum_t::middle_right);
            spr.drawString(buf, 236, 24);
        }
    } else {
        spr.setFont(&fonts::DejaVu72);
        spr.setTextDatum(textdatum_t::middle_center);
        spr.setTextColor(C_PANEL);
        spr.drawString("--", 120, 50);
    }

    drawMeter(needle, shown, color);

    // Línea de cents y frecuencia.
    spr.setFont(&fonts::lgfxJapanGothicP_12);
    if (shown) {
        char buf[24];
        int c = (int)lroundf(cents);
        if (fabsf(cents) > 999) snprintf(buf, sizeof(buf), "%s", cents > 0 ? "muy alto" : "muy bajo");
        else snprintf(buf, sizeof(buf), "%+d cents", c);
        spr.setTextDatum(textdatum_t::middle_left);
        spr.setTextColor(color);
        spr.drawString(buf, 4, 107);

        spr.setTextDatum(textdatum_t::middle_center);
        if (tuned) {
            spr.setTextColor(C_GREEN);
            spr.drawString("¡Afinada!", 120, 107);
        }

        snprintf(buf, sizeof(buf), "%.1f Hz", lastFreq);
        spr.setTextDatum(textdatum_t::middle_right);
        spr.setTextColor(C_DIM);
        spr.drawString(buf, 236, 107);
    } else {
        spr.setTextDatum(textdatum_t::middle_center);
        spr.setTextColor(C_DIM);
        spr.drawString("Tocá una cuerda", 120, 107);
    }

    drawStrings(shown ? (int)lroundf(midiSmooth) : -1);
    spr.pushSprite(0, 0);
}

// --- Tono de referencia ---------------------------------------------------------------
// El parlante y el micrófono comparten el bus I2S: hay que apagar uno para
// usar el otro. Diente de sierra para que las notas graves se oigan en el
// parlante chico (se escuchan los armónicos).
static const uint8_t SAW[32] = {
    0, 8, 16, 24, 33, 41, 49, 57, 66, 74, 82, 90, 99, 107, 115, 123,
    132, 140, 148, 156, 165, 173, 181, 189, 198, 206, 214, 222, 231, 239, 247, 255,
};

static void playReference()
{
    float m = targetMidi();
    if (chromatic() && !haveNote) m = 69; // sin nota todavía: La 440
    float f = midiToFreq(m);

    M5Cardputer.Mic.end();
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(180);
    M5Cardputer.Speaker.tone(f, 2000, 0, true, SAW, sizeof(SAW));

    spr.fillRect(40, 100, 160, 14, C_BG);
    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextDatum(textdatum_t::middle_center);
    spr.setTextColor(C_ACCENT);
    char buf[32];
    snprintf(buf, sizeof(buf), "♪ %s%d  %.1f Hz", noteName((int)m), octave((int)m), f);
    spr.drawString(buf, 120, 107);
    spr.pushSprite(0, 0);

    while (M5Cardputer.Speaker.isPlaying()) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
        delay(10);
    }
    M5Cardputer.Speaker.stop();
    startMic();
    haveNote = false;
    histN = 0;
}

// --- Teclado -------------------------------------------------------------------------------
static void changeTuning(int d)
{
    tuningIdx = (tuningIdx + d + NUM_TUNINGS) % NUM_TUNINGS;
    manualString = -1;
    autoString = 0;
    tunedMask = 0;
    inTuneSince = 0;
    savePrefs();
}

static void handleKeys()
{
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto st = M5Cardputer.Keyboard.keysState();
    if (st.enter) {
        manualString = -1;
        return;
    }
    if (st.space) {
        playReference();
        return;
    }
    for (char c : st.word) {
        switch (c) {
        case ';': changeTuning(-1); break;
        case '.': changeTuning(1); break;
        case ',':
        case '/':
            if (!chromatic()) {
                int n = tuning().n;
                int cur = targetString();
                manualString = (cur + (c == '/' ? 1 : n - 1)) % n;
                inTuneSince = 0;
            }
            break;
        case '-':
        case '_':
            if (a4 > 430) --a4, savePrefs();
            break;
        case '=':
        case '+':
            if (a4 < 450) ++a4, savePrefs();
            break;
        case 'n':
        case 'N':
            doremi = !doremi;
            savePrefs();
            break;
        }
    }
}

// --- Arduino ----------------------------------------------------------------------------------
void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(160);

    prefs.begin("tuner", false);
    tuningIdx = prefs.getUChar("tuning", 1);
    if (tuningIdx >= NUM_TUNINGS) tuningIdx = 1;
    a4 = prefs.getUShort("a4", 440);
    if (a4 < 430 || a4 > 450) a4 = 440;
    doremi = prefs.getBool("doremi", false);

    spr.setColorDepth(16);
    spr.createSprite(240, 135);

    // Portada corta mientras arranca el micrófono.
    spr.fillSprite(C_BG);
    spr.setFont(&fonts::DejaVu40);
    spr.setTextDatum(textdatum_t::middle_center);
    spr.setTextColor(C_GREEN);
    spr.drawString("Afinador", 120, 56);
    spr.setFont(&fonts::lgfxJapanGothicP_12);
    spr.setTextColor(C_DIM);
    spr.drawString("v" APP_VERSION, 120, 92);
    spr.pushSprite(0, 0);

    startMic();
    delay(400);
}

void loop()
{
    M5Cardputer.update();
    handleKeys();

    const int16_t *chunk;
    if (!readChunk(chunk)) return;
    for (size_t i = 0; i < CHUNK; ++i) {
        ring[ringPos] = chunk[i];
        ringPos = (ringPos + 1) % FRAME;
    }

    float rms = 0;
    float f = detectPitch(ring, rms);
    float lv = std::min(1.0f, std::max(0.0f, (20 * log10f(rms + 1) - 20) / 60)); // ~-70..-10 dBFS
    level += (lv > level ? 0.6f : 0.15f) * (lv - level);

    if (f > 0) onPitch(f);
    else if (haveNote && millis() - lastHeard > 250) inTuneSince = 0;
    render();
}
