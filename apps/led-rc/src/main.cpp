// Control remoto IR para luces LED RGB genéricas (las de los mandos de 24 y 44 teclas)
// para M5Stack Cardputer-Adv.
//
// Estos mandos usan NEC a 38 kHz:
//   24 teclas: dirección 0xEF00, códigos 0x00F7xxxx
//   44 teclas: dirección 0x00FF, códigos 0x00FFxxxx
// Los valores están en el orden del IRremote viejo (se mandan del bit más alto al más bajo).
// Apuntá el extremo superior del Cardputer (LED IR en G44) a la luz.
//
// Teclas:
//   ; . , /       mover por la grilla
//   enter         enviar la tecla (brillo y R/G/B se repiten mientras la mantenés)
//   o  f          prender / apagar          - =   brillo - / +
//   r g b w       rojo / verde / azul / blanco
//   tab           cambiar entre el mando de 24 y el de 44 teclas

#include <M5Cardputer.h>
#include <Preferences.h>
#include <esp_timer.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HUD_H = 15;

static constexpr int IR_PIN = 44;
static constexpr int IR_CH = 0;
static constexpr int IR_RES = 8;
static constexpr uint32_t DUTY = 85;          // ~33 %
static constexpr uint32_t REPEAT_MS = 108;    // período de una trama NEC
static constexpr uint32_t HOLD_DELAY_MS = 350;

// --- Mandos ------------------------------------------------------------------------
struct Key {
    const char *label;
    uint16_t code;     // 16 bits bajos del código NEC
    uint8_t r, g, b;   // color del botón (0,0,0 = gris, tecla de función)
    bool hold;         // se repite mientras está apretada
    const char *name;  // nombre largo para el panel
};

struct Remote {
    const char *label;
    uint32_t prefix;   // 16 bits altos
    int cols;
    int count;
    const Key *keys;
};

#define FN(l, c, hold, n) {l, c, 0, 0, 0, hold, n}
#define CL(l, c, r, g, b, n) {l, c, r, g, b, false, n}

static const Key KEYS24[] = {
    FN("B+", 0x00FF, true, "Brillo +"), FN("B-", 0x807F, true, "Brillo -"),
    FN("OFF", 0x40BF, false, "Apagar"), FN("ON", 0xC03F, false, "Prender"),
    CL("R", 0x20DF, 255, 0, 0, "Rojo"), CL("G", 0xA05F, 0, 255, 0, "Verde"),
    CL("B", 0x609F, 0, 0, 255, "Azul"), CL("W", 0xE01F, 255, 255, 255, "Blanco"),
    CL("", 0x10EF, 255, 80, 0, "Naranja"), CL("", 0x906F, 0, 220, 90, "Verde claro"),
    CL("", 0x50AF, 40, 70, 255, "Azul claro"), FN("FLSH", 0xD02F, false, "Flash"),
    CL("", 0x30CF, 255, 150, 0, "Ambar"), CL("", 0xB04F, 0, 200, 200, "Cian"),
    CL("", 0x708F, 120, 0, 200, "Violeta"), FN("STRB", 0xF00F, false, "Strobe"),
    CL("", 0x08F7, 255, 210, 0, "Amarillo oscuro"), CL("", 0x8877, 0, 150, 150, "Turquesa"),
    CL("", 0x48B7, 190, 0, 170, "Magenta"), FN("FADE", 0xC837, false, "Fade"),
    CL("", 0x28D7, 255, 255, 0, "Amarillo"), CL("", 0xA857, 0, 110, 190, "Celeste"),
    CL("", 0x6897, 255, 90, 160, "Rosa"), FN("SMTH", 0xE817, false, "Smooth"),
};

static const Key KEYS44[] = {
    FN("B+", 0x3AC5, true, "Brillo +"), FN("B-", 0xBA45, true, "Brillo -"),
    FN("PLAY", 0x827D, false, "Play / Pausa"), FN("PWR", 0x02FD, false, "Prender / apagar"),
    CL("R", 0x1AE5, 255, 0, 0, "Rojo"), CL("G", 0x9A65, 0, 255, 0, "Verde"),
    CL("B", 0xA25D, 0, 0, 255, "Azul"), CL("W", 0x22DD, 255, 255, 255, "Blanco"),
    CL("", 0x2AD5, 255, 80, 0, "Naranja"), CL("", 0xAA55, 0, 220, 90, "Verde claro"),
    CL("", 0x926D, 40, 70, 255, "Azul claro"), CL("", 0x12ED, 255, 200, 200, "Blanco rosado"),
    CL("", 0x0AF5, 255, 130, 0, "Naranja claro"), CL("", 0x8A75, 0, 200, 150, "Turquesa"),
    CL("", 0xB24D, 100, 0, 200, "Violeta"), CL("", 0x32CD, 255, 230, 230, "Blanco calido"),
    CL("", 0x38C7, 255, 170, 0, "Ambar"), CL("", 0xB847, 0, 170, 180, "Cian"),
    CL("", 0x7887, 180, 0, 180, "Magenta"), CL("", 0xF807, 220, 240, 255, "Blanco frio"),
    CL("", 0x18E7, 255, 230, 0, "Amarillo"), CL("", 0x9867, 0, 120, 200, "Celeste"),
    CL("", 0x58A7, 255, 60, 150, "Rosa"), CL("", 0xD827, 180, 220, 255, "Blanco celeste"),
    FN("R+", 0x28D7, true, "Rojo +"), FN("G+", 0xA857, true, "Verde +"),
    FN("B+", 0x6897, true, "Azul +"), FN("QCK", 0xE817, false, "Rapido"),
    FN("R-", 0x08F7, true, "Rojo -"), FN("G-", 0x8877, true, "Verde -"),
    FN("B-", 0x48B7, true, "Azul -"), FN("SLOW", 0xC837, false, "Lento"),
    FN("DIY1", 0x30CF, false, "DIY 1"), FN("DIY2", 0xB04F, false, "DIY 2"),
    FN("DIY3", 0x708F, false, "DIY 3"), FN("AUTO", 0xF00F, false, "Auto"),
    FN("DIY4", 0x10EF, false, "DIY 4"), FN("DIY5", 0x906F, false, "DIY 5"),
    FN("DIY6", 0x50AF, false, "DIY 6"), FN("FLSH", 0xD02F, false, "Flash"),
    FN("JMP3", 0x20DF, false, "Salto 3"), FN("JMP7", 0xA05F, false, "Salto 7"),
    FN("FAD3", 0x609F, false, "Fade 3"), FN("FAD7", 0xE01F, false, "Fade 7"),
};

static const Remote REMOTES[2] = {
    {"24 teclas", 0x00F7, 4, sizeof(KEYS24) / sizeof(Key), KEYS24},
    {"44 teclas", 0x00FF, 4, sizeof(KEYS44) / sizeof(Key), KEYS44},
};

// --- Estado ---------------------------------------------------------------------------
static M5Canvas canvas(&M5Cardputer.Display);
static Preferences prefs;

static int remoteIdx;
static int cursor[2];
static bool dirty = true;

static const Key *heldKey;       // tecla que se está repitiendo
static uint32_t heldSince, lastFrame;
static uint32_t sentCount;
static uint32_t flashUntil;      // animación de envío
static const Key *lastKey;

static uint16_t C_BG, C_HUD, C_TEXT, C_DIM, C_ACCENT, C_IR, C_GRAY;

// --- Emisión IR --------------------------------------------------------------------------
static inline void waitUntil(int64_t t)
{
    while (esp_timer_get_time() < t) {
    }
}

static void irOn() { ledcWrite(IR_CH, DUTY); }
static void irOff() { ledcWrite(IR_CH, 0); }

static int64_t pulse(int64_t t, uint32_t markUs, uint32_t spaceUs)
{
    irOn();
    t += markUs;
    waitUntil(t);
    irOff();
    t += spaceUs;
    waitUntil(t);
    return t;
}

static void sendNec(uint32_t v)
{
    int64_t t = esp_timer_get_time();
    t = pulse(t, 9000, 4500);
    for (int i = 31; i >= 0; i--) t = pulse(t, 560, (v >> i) & 1 ? 1690 : 560);
    pulse(t, 560, 0);
}

static void sendRepeat()
{
    int64_t t = esp_timer_get_time();
    t = pulse(t, 9000, 2250);
    pulse(t, 560, 0);
}

static void fire(const Key *k, bool repeat)
{
    if (repeat) {
        sendRepeat();
    } else {
        sendNec(((uint32_t)REMOTES[remoteIdx].prefix << 16) | k->code);
        sentCount++;
        lastKey = k;
    }
    lastFrame = millis();
    flashUntil = lastFrame + 150;
    dirty = true;
}

static int findByLabel(const char *label)
{
    const Remote &r = REMOTES[remoteIdx];
    for (int i = 0; i < r.count; i++) {
        if (!strcmp(r.keys[i].label, label)) return i;
    }
    return -1;
}

// Atajo: lleva el cursor a la tecla y la envía.
static void shortcut(const char *label)
{
    int i = findByLabel(label);
    if (i < 0) return;
    cursor[remoteIdx] = i;
    heldKey = &REMOTES[remoteIdx].keys[i];
    heldSince = millis();
    fire(heldKey, false);
}

// --- Dibujo --------------------------------------------------------------------------------
static void drawHud()
{
    canvas.fillRect(0, 0, SCREEN_W, HUD_H, C_HUD);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(C_ACCENT, C_HUD);
    canvas.drawString("LED RC", 4, HUD_H / 2);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_TEXT, C_HUD);
    canvas.drawString(REMOTES[remoteIdx].label, SCREEN_W / 2, HUD_H / 2);
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(C_DIM, C_HUD);
    canvas.drawString("v" APP_VERSION, SCREEN_W - 4, HUD_H / 2);
}

static void drawFooter(const char *text)
{
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(text, SCREEN_W / 2, SCREEN_H - 2);
}

static void drawKey(const Key &k, int x, int y, int w, int h, bool sel)
{
    uint16_t fill = (k.r | k.g | k.b) ? canvas.color565(k.r, k.g, k.b) : C_GRAY;
    canvas.fillRoundRect(x, y, w, h, 4, fill);
    if (sel) {
        canvas.drawRoundRect(x - 2, y - 2, w + 4, h + 4, 6, C_ACCENT);
        canvas.drawRoundRect(x - 1, y - 1, w + 2, h + 2, 5, C_ACCENT);
    }
    if (k.label[0]) {
        bool darkText = (k.r + k.g + k.b) > 380;
        canvas.setFont(&fonts::Font0);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor((k.r | k.g | k.b) ? (darkText ? C_BG : C_TEXT) : C_TEXT, fill);
        canvas.drawString(k.label, x + w / 2, y + h / 2);
    }
}

static void drawIr(int cx, int cy, bool on)
{
    canvas.fillRoundRect(cx - 14, cy - 8, 28, 16, 3, C_HUD);
    canvas.fillCircle(cx, cy, 4, on ? C_IR : C_DIM);
    if (on) {
        for (int i = 1; i <= 3; i++) canvas.drawArc(cx, cy - 8, 6 + i * 6, 5 + i * 6, 235, 305, C_IR);
    }
}

static void render()
{
    const Remote &r = REMOTES[remoteIdx];
    int cur = cursor[remoteIdx];
    canvas.fillScreen(C_BG);
    drawHud();

    // Grilla con scroll vertical: 6 filas visibles.
    const int cellW = 40, cellH = 17, gapX = 4, gapY = 3, x0 = 8, y0 = 21, rowsShown = 6;
    int totalRows = (r.count + r.cols - 1) / r.cols;
    int curRow = cur / r.cols;
    int firstRow = constrain(curRow - rowsShown / 2, 0, max(0, totalRows - rowsShown));
    for (int row = 0; row < rowsShown && firstRow + row < totalRows; row++) {
        for (int c = 0; c < r.cols; c++) {
            int i = (firstRow + row) * r.cols + c;
            if (i >= r.count) break;
            drawKey(r.keys[i], x0 + c * (cellW + gapX), y0 + row * (cellH + gapY), cellW, cellH, i == cur);
        }
    }
    if (totalRows > rowsShown) {  // barra de scroll
        int trackH = rowsShown * (cellH + gapY) - gapY;
        int barH = max(8, trackH * rowsShown / totalRows);
        int barY = y0 + (trackH - barH) * firstRow / (totalRows - rowsShown);
        canvas.fillRect(x0 + r.cols * (cellW + gapX) - 1, y0, 2, trackH, C_HUD);
        canvas.fillRect(x0 + r.cols * (cellW + gapX) - 1, barY, 2, barH, C_DIM);
    }

    // Panel derecho.
    const Key &k = r.keys[cur];
    const int px = 192;
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("TECLA", px, 22);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(k.name, px, 33);
    char code[12];
    snprintf(code, sizeof(code), "%04X%04X", r.prefix, k.code);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(code, px, 45);
    if (k.hold) {
        canvas.setTextColor(C_ACCENT, C_BG);
        canvas.drawString("mantener", px, 57);
    }
    drawIr(px + 22, 88, millis() < flashUntil);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.setTextDatum(top_center);
    canvas.drawString(String(sentCount) + " enviados", px + 22, 102);

    drawFooter("; . , / mover  enter enviar  tab mando");
    canvas.pushSprite(0, 0);
}

// --- Entrada ---------------------------------------------------------------------------------
static bool hasChar(const Keyboard_Class::KeysState &ks, const char *chars)
{
    for (char c : ks.word) {
        if (strchr(chars, c)) return true;
    }
    return false;
}

static void move(int dc, int dr)
{
    const Remote &r = REMOTES[remoteIdx];
    int &cur = cursor[remoteIdx];
    int rows = (r.count + r.cols - 1) / r.cols;
    int c = cur % r.cols, row = cur / r.cols;
    c = (c + dc + r.cols) % r.cols;
    row = (row + dr + rows) % rows;
    int n = row * r.cols + c;
    cur = n < r.count ? n : r.count - 1;
    prefs.putUChar(remoteIdx ? "cur44" : "cur24", cur);
}

static void press(const Keyboard_Class::KeysState &ks)
{
    if (hasChar(ks, "\t") || ks.tab) {
        remoteIdx ^= 1;
        prefs.putUChar("remote", remoteIdx);
        heldKey = nullptr;
        return;
    }
    if (hasChar(ks, ";")) move(0, -1);
    else if (hasChar(ks, ".")) move(0, 1);
    else if (hasChar(ks, ",")) move(-1, 0);
    else if (hasChar(ks, "/")) move(1, 0);
    else if (ks.enter) {
        heldKey = &REMOTES[remoteIdx].keys[cursor[remoteIdx]];
        heldSince = millis();
        fire(heldKey, false);
    } else if (hasChar(ks, "oO")) shortcut(remoteIdx ? "PWR" : "ON");
    else if (hasChar(ks, "fF")) shortcut(remoteIdx ? "PWR" : "OFF");
    else if (hasChar(ks, "-_")) shortcut("B-");
    else if (hasChar(ks, "=+")) shortcut("B+");
    else if (hasChar(ks, "rR")) shortcut("R");
    else if (hasChar(ks, "gG")) shortcut("G");
    else if (hasChar(ks, "bB")) shortcut("B");
    else if (hasChar(ks, "wW")) shortcut("W");
}

static void handleKeys()
{
    M5Cardputer.update();
    bool down = M5Cardputer.Keyboard.isPressed();
    if (M5Cardputer.Keyboard.isChange() && down) {
        press(M5Cardputer.Keyboard.keysState());
        dirty = true;
    }
    if (!down) {
        heldKey = nullptr;
    } else if (heldKey && heldKey->hold) {
        uint32_t now = millis();
        if (now - heldSince >= HOLD_DELAY_MS && now - lastFrame >= REPEAT_MS) fire(heldKey, true);
    }
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    canvas.setColorDepth(16);
    canvas.createSprite(SCREEN_W, SCREEN_H);

    C_BG = canvas.color565(12, 12, 16);
    C_HUD = canvas.color565(34, 30, 38);
    C_TEXT = canvas.color565(225, 225, 230);
    C_DIM = canvas.color565(120, 120, 130);
    C_ACCENT = canvas.color565(255, 190, 40);
    C_IR = canvas.color565(200, 60, 255);
    C_GRAY = canvas.color565(70, 70, 82);

    prefs.begin("led-rc", false);
    remoteIdx = min<uint8_t>(prefs.getUChar("remote", 0), 1);
    cursor[0] = min<int>(prefs.getUChar("cur24", 0), REMOTES[0].count - 1);
    cursor[1] = min<int>(prefs.getUChar("cur44", 0), REMOTES[1].count - 1);

    ledcSetup(IR_CH, 38000, IR_RES);
    ledcAttachPin(IR_PIN, IR_CH);
    irOff();

    render();
}

void loop()
{
    handleKeys();
    if (flashUntil && millis() >= flashUntil) {
        flashUntil = 0;
        dirty = true;
    }
    if (dirty) {
        render();
        dirty = false;
    }
    delay(2);
}
