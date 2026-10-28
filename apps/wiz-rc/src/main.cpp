// Control remoto UDP de luces WiZ para M5Stack Cardputer-Adv
//
// Las lámparas WiZ escuchan JSON por UDP en el puerto 38899 (sin autenticación),
// así que alcanza con estar en la misma red WiFi:
//   {"method":"getPilot","params":{}}                                  estado
//   {"id":1,"method":"setPilot","params":{"state":true,"r":255,"g":0,"b":0,"dimming":80}}
//   {"id":1,"method":"setPilot","params":{"temp":2700,"dimming":50}}   blanco
//   {"id":1,"method":"setPilot","params":{"sceneId":4}}                escena
//
// Teclas:
//   red WiFi        ; . elegir, enter conectar, r buscar otra vez
//   clave / IP      escribir, enter aceptar, del borrar, tab cancelar
//   lámparas        ; . elegir, enter controlar, r buscar, i agregar por IP, w cambiar de red
//   control         tab modo (color / blanco / escena), space prender-apagar
//                   ; . campo, , / valor (mantener acelera), 1-8 colores, del volver

#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <algorithm>
#include <vector>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HUD_H = 15;

static constexpr uint16_t WIZ_PORT = 38899;
static constexpr uint32_t DISCOVER_MS = 3500;
static constexpr uint32_t CONNECT_MS = 15000;
static constexpr uint32_t SEND_MIN_MS = 70;   // límite de envío mientras se ajusta un valor
static constexpr int MAX_BULBS = 12;
static constexpr int MAX_NETS = 12;

// --- Datos ----------------------------------------------------------------------------
static const char *SCENES[32] = {
    "Ocean", "Romance", "Sunset", "Party", "Fireplace", "Cozy", "Forest", "Pastel colors",
    "Wake up", "Bedtime", "Warm white", "Daylight", "Cool white", "Night light", "Focus", "Relax",
    "True colors", "TV time", "Plantgrowth", "Spring", "Summer", "Fall", "Deep dive", "Jungle",
    "Mojito", "Club", "Christmas", "Halloween", "Candlelight", "Golden white", "Pulse", "Steampunk",
};

struct Preset {
    const char *name;
    uint16_t hue;
};
static const Preset PRESETS[8] = {{"Rojo", 0},     {"Naranja", 25}, {"Amarillo", 55}, {"Verde", 120},
                                  {"Cian", 180},   {"Azul", 240},   {"Violeta", 275}, {"Rosa", 320}};

enum class Screen { Connecting, Networks, Input, Bulbs, Control };
enum class Mode : uint8_t { Color, White, Scene };
enum class InputFor { Password, Ip };

struct Net {
    String ssid;
    int rssi;
    bool open;
};

struct Bulb {
    IPAddress ip;
    String mac;
    int rssi;
};

// --- Estado ------------------------------------------------------------------------------
static M5Canvas canvas(&M5Cardputer.Display);
static Preferences prefs;
static WiFiUDP udp;

static Screen screen = Screen::Connecting;
static bool dirty = true;
static String notice;

static std::vector<Net> nets;
static int netSel;
static bool netScanning;
static String ssid, pass;
static uint32_t connectStart;

static String inputBuf;
static InputFor inputFor;
static String inputTitle;

static std::vector<Bulb> bulbs;
static int bulbSel;      // 0 = todas (si hay más de una) o la primera
static bool discovering;
static uint32_t discoverStart, lastProbe;
static std::vector<IPAddress> targets;
static String targetName;

static Mode mode = Mode::Color;
static int field;            // campo seleccionado dentro del modo
static bool lightOn = true;
static int hue = 0, sat = 100, dim = 100, temp = 2700, scene = 4;
static bool pending;         // hay un valor por enviar
static uint32_t lastSendMs;
static uint32_t lastUserMs;
static int heldDir;          // -1, 0, +1 (tecla , / mantenida)
static uint32_t heldSince, heldLast;
static uint32_t packets;

static uint16_t C_BG, C_HUD, C_TEXT, C_DIM, C_ACCENT, C_RED, C_ON, C_OFF;

// --- Color -------------------------------------------------------------------------------
static void hsv2rgb(int h, int s, int v, uint8_t &r, uint8_t &g, uint8_t &b)
{
    h = ((h % 360) + 360) % 360;
    float S = s / 100.0f, V = v / 100.0f;
    float c = V * S, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = V - c;
    float rr, gg, bb;
    if (h < 60) rr = c, gg = x, bb = 0;
    else if (h < 120) rr = x, gg = c, bb = 0;
    else if (h < 180) rr = 0, gg = c, bb = x;
    else if (h < 240) rr = 0, gg = x, bb = c;
    else if (h < 300) rr = x, gg = 0, bb = c;
    else rr = c, gg = 0, bb = x;
    r = (rr + m) * 255;
    g = (gg + m) * 255;
    b = (bb + m) * 255;
}

// Aproximación de color para una temperatura de blanco (algoritmo de Tanner Helland).
static void kelvin2rgb(int k, uint8_t &r, uint8_t &g, uint8_t &b)
{
    float t = k / 100.0f, rr, gg, bb;
    rr = t <= 66 ? 255 : 329.698727446f * powf(t - 60, -0.1332047592f);
    gg = t <= 66 ? 99.4708025861f * logf(t) - 161.1195681661f : 288.1221695283f * powf(t - 60, -0.0755148492f);
    bb = t >= 66 ? 255 : (t <= 19 ? 0 : 138.5177312231f * logf(t - 10) - 305.0447927307f);
    r = constrain((int)rr, 0, 255);
    g = constrain((int)gg, 0, 255);
    b = constrain((int)bb, 0, 255);
}

// --- Red WiZ ------------------------------------------------------------------------------------
static void sendTo(IPAddress ip, const char *json)
{
    udp.beginPacket(ip, WIZ_PORT);
    udp.write((const uint8_t *)json, strlen(json));
    udp.endPacket();
    packets++;
}

static void sendAll(const char *json)
{
    for (auto &ip : targets) sendTo(ip, json);
}

static void sendState()
{
    char buf[160];
    if (!lightOn) {
        snprintf(buf, sizeof(buf), "{\"id\":1,\"method\":\"setPilot\",\"params\":{\"state\":false}}");
    } else if (mode == Mode::Color) {
        uint8_t r, g, b;
        hsv2rgb(hue, sat, 100, r, g, b);
        snprintf(buf, sizeof(buf),
                 "{\"id\":1,\"method\":\"setPilot\",\"params\":{\"state\":true,\"r\":%u,\"g\":%u,\"b\":%u,\"dimming\":%d}}",
                 r, g, b, dim);
    } else if (mode == Mode::White) {
        snprintf(buf, sizeof(buf),
                 "{\"id\":1,\"method\":\"setPilot\",\"params\":{\"state\":true,\"temp\":%d,\"dimming\":%d}}", temp, dim);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"id\":1,\"method\":\"setPilot\",\"params\":{\"state\":true,\"sceneId\":%d,\"dimming\":%d}}", scene + 1,
                 dim);
    }
    sendAll(buf);
    pending = false;
    lastSendMs = millis();
}

static void changed()
{
    pending = true;
    lastUserMs = millis();
    dirty = true;
}

static long jsonInt(const char *s, const char *key, long def)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(s, pat);
    return p ? atol(p + strlen(pat)) : def;
}

static String jsonStr(const char *s, const char *key)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(s, pat);
    if (!p) return "";
    p += strlen(pat);
    const char *e = strchr(p, '"');
    return e ? String(p).substring(0, e - p) : String("");
}

static void probe()
{
    const char *msg = "{\"method\":\"getPilot\",\"params\":{}}";
    sendTo(WiFi.broadcastIP(), msg);
    sendTo(IPAddress(255, 255, 255, 255), msg);
    lastProbe = millis();
}

static void startDiscovery()
{
    bulbs.clear();
    bulbSel = 0;
    discovering = true;
    discoverStart = millis();
    notice = "";
    probe();
    dirty = true;
}

static void addBulb(IPAddress ip, const String &mac, int rssi)
{
    for (auto &b : bulbs) {
        if (b.ip == ip) {
            if (mac.length()) b.mac = mac;
            b.rssi = rssi;
            return;
        }
    }
    if ((int)bulbs.size() < MAX_BULBS) bulbs.push_back({ip, mac, rssi});
    dirty = true;
}

// Lee lo que llegó por UDP: respuestas al descubrimiento o al estado.
static void pollUdp()
{
    int n;
    while ((n = udp.parsePacket()) > 0) {
        char buf[512];
        int len = udp.read(buf, sizeof(buf) - 1);
        if (len <= 0) continue;
        buf[len] = 0;
        IPAddress from = udp.remoteIP();
        if (screen == Screen::Bulbs && discovering && strstr(buf, "\"result\"")) {
            addBulb(from, jsonStr(buf, "mac"), (int)jsonInt(buf, "rssi", -100));
        } else if (screen == Screen::Control && !targets.empty() && from == targets[0] && strstr(buf, "\"getPilot\"") &&
                   millis() - lastUserMs > 800) {
            lightOn = strstr(buf, "\"state\":true") != nullptr;
            long d = jsonInt(buf, "dimming", dim);
            dim = constrain((int)d, 10, 100);
            long t = jsonInt(buf, "temp", 0);
            if (t >= 2200 && t <= 6500) temp = t;
            dirty = true;
        }
    }
}

static String bulbLabel(const Bulb &b)
{
    String m = b.mac.length() >= 6 ? b.mac.substring(b.mac.length() - 6) : String("");
    m.toUpperCase();
    return "WiZ " + (m.length() ? m : String("?")) + "  " + b.ip.toString();
}

static void openControl(const std::vector<IPAddress> &ips, const String &name)
{
    targets = ips;
    targetName = name;
    mode = Mode::Color;
    field = 0;
    lightOn = true;
    pending = false;
    heldDir = 0;
    lastUserMs = 0;
    screen = Screen::Control;
    discovering = false;
    sendAll("{\"method\":\"getPilot\",\"params\":{}}");
    dirty = true;
}

// --- WiFi -------------------------------------------------------------------------------------------
static void startScanNets()
{
    nets.clear();
    netSel = 0;
    netScanning = true;
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    dirty = true;
}

static void collectNets()
{
    int n = WiFi.scanComplete();
    if (n < 0) return;
    netScanning = false;
    nets.clear();
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        if (!s.length()) continue;
        bool dup = false;
        for (auto &x : nets) dup |= x.ssid == s;
        if (!dup) nets.push_back({s, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
    }
    std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
    if ((int)nets.size() > MAX_NETS) nets.resize(MAX_NETS);
    WiFi.scanDelete();
    dirty = true;
}

static void connectWifi()
{
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());
    connectStart = millis();
    screen = Screen::Connecting;
    dirty = true;
}

static void goNetworks()
{
    screen = Screen::Networks;
    startScanNets();
}

static void beginInput(InputFor f, const String &title)
{
    inputFor = f;
    inputTitle = title;
    inputBuf = "";
    screen = Screen::Input;
    dirty = true;
}

// --- Dibujo -----------------------------------------------------------------------------------------------
static void drawHud(const char *center, const char *right)
{
    canvas.fillRect(0, 0, SCREEN_W, HUD_H, C_HUD);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(C_ACCENT, C_HUD);
    canvas.drawString("WIZ RC", 4, HUD_H / 2);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_TEXT, C_HUD);
    canvas.drawString(center, SCREEN_W / 2 + 10, HUD_H / 2);
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(C_DIM, C_HUD);
    canvas.drawString(right, SCREEN_W - 4, HUD_H / 2);
}

static void drawFooter(const char *text)
{
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(text, SCREEN_W / 2, SCREEN_H - 2);
}

static void drawBars(int x, int y, int rssi)
{
    int bars = constrain((rssi + 90) / 10, 0, 4);
    for (int b = 0; b < 4; b++) {
        int h = 2 + b * 2;
        canvas.fillRect(x + b * 4, y + 9 - h, 3, h, b < bars ? C_ON : C_OFF);
    }
}

static void drawCentered(const char *title, const char *line, uint16_t titleColor)
{
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(titleColor, C_BG);
    canvas.drawString(title, SCREEN_W / 2, 58);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(line, SCREEN_W / 2, 80);
}

static void drawConnecting()
{
    drawHud("WiFi", "v" APP_VERSION);
    drawCentered("Conectando", ssid.c_str(), C_ACCENT);
    int w = (millis() - connectStart) * 200 / CONNECT_MS;
    canvas.drawRect(20, 98, 200, 6, C_DIM);
    canvas.fillRect(21, 99, constrain(w, 0, 198), 4, C_ACCENT);
    drawFooter("del cambiar de red");
}

static void drawNetworks()
{
    drawHud("Redes WiFi", netScanning ? "buscando..." : "");
    canvas.setFont(&fonts::Font0);
    if (nets.empty()) {
        drawCentered(netScanning ? "Buscando redes..." : "No encontre redes", "r para buscar otra vez", C_TEXT);
    }
    int first = constrain(netSel - 5, 0, max(0, (int)nets.size() - 8));
    for (int i = first; i < (int)nets.size() && i < first + 8; i++) {
        int y = 19 + (i - first) * 12;
        bool sel = i == netSel;
        if (sel) canvas.fillRect(4, y - 1, SCREEN_W - 8, 11, C_HUD);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(sel ? C_ACCENT : C_TEXT, sel ? C_HUD : C_BG);
        String name = nets[i].ssid.length() > 24 ? nets[i].ssid.substring(0, 24) : nets[i].ssid;
        canvas.drawString((sel ? "> " : "  ") + name, 8, y + 1);
        if (!nets[i].open) {
            canvas.setTextColor(C_DIM, sel ? C_HUD : C_BG);
            canvas.drawString("*", SCREEN_W - 52, y + 1);
        }
        drawBars(SCREEN_W - 40, y, nets[i].rssi);
    }
    if (notice.length()) {
        canvas.setTextDatum(bottom_center);
        canvas.setTextColor(C_RED, C_BG);
        canvas.drawString(notice, SCREEN_W / 2, SCREEN_H - 13);
    }
    drawFooter("enter elegir  r buscar");
}

static void drawInput()
{
    drawHud(inputFor == InputFor::Password ? "Clave" : "IP de la lampara", "");
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(inputTitle, 10, 30);
    canvas.fillRoundRect(10, 48, 220, 26, 5, C_HUD);
    canvas.drawRoundRect(10, 48, 220, 26, 5, C_ACCENT);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT, C_HUD);
    String shown = inputBuf;
    while (shown.length() > 1 && canvas.textWidth(shown + "_") > 208) shown.remove(0, 1);
    canvas.setTextDatum(middle_left);
    canvas.drawString(shown + ((millis() / 500) % 2 ? "_" : " "), 16, 61);
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(String(inputBuf.length()) + " caracteres", 10, 80);
    if (notice.length()) {
        canvas.setTextColor(C_RED, C_BG);
        canvas.drawString(notice, 10, 94);
    }
    drawFooter("enter aceptar  del borrar  tab cancelar");
}

static void drawBulbs()
{
    String ipStr = WiFi.localIP().toString();
    drawHud("Lamparas", discovering ? "buscando..." : ipStr.c_str());
    canvas.setFont(&fonts::Font0);
    int rows = bulbs.size() + (bulbs.size() > 1 ? 1 : 0);
    if (!rows) {
        drawCentered(discovering ? "Buscando lamparas..." : "No encontre lamparas",
                     discovering ? "en la red " : "prendelas y apreta r  (o i: IP)", discovering ? C_TEXT : C_RED);
    }
    for (int i = 0; i < rows; i++) {
        int y = 19 + i * 12;
        bool sel = i == bulbSel;
        if (sel) canvas.fillRect(4, y - 1, SCREEN_W - 8, 11, C_HUD);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(sel ? C_ACCENT : C_TEXT, sel ? C_HUD : C_BG);
        bool all = bulbs.size() > 1 && i == 0;
        String label = all ? "Todas (" + String(bulbs.size()) + ")" : bulbLabel(bulbs[i - (bulbs.size() > 1 ? 1 : 0)]);
        canvas.drawString((sel ? "> " : "  ") + label, 8, y + 1);
        if (!all) drawBars(SCREEN_W - 24, y, bulbs[i - (bulbs.size() > 1 ? 1 : 0)].rssi);
    }
    if (notice.length()) {
        canvas.setTextDatum(bottom_center);
        canvas.setTextColor(C_RED, C_BG);
        canvas.drawString(notice, SCREEN_W / 2, SCREEN_H - 13);
    }
    drawFooter("enter control  r buscar  i IP  w red");
}

// Barra con degradé o relleno; value 0..1.
static void drawSlider(int x, int y, int w, int h, float value, bool sel, const char *label, const String &text,
                       int kind /*0 tono 1 sat 2 brillo 3 temp*/)
{
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(sel ? C_ACCENT : C_DIM, C_BG);
    canvas.drawString(label, x, y);
    canvas.setTextDatum(top_right);
    canvas.setTextColor(sel ? C_TEXT : C_DIM, C_BG);
    canvas.drawString(text, x + w, y);
    int by = y + 11;
    for (int i = 0; i < w; i++) {
        float f = (float)i / (w - 1);
        uint8_t r, g, b;
        switch (kind) {
            case 0: hsv2rgb((int)(f * 359), 100, 100, r, g, b); break;
            case 1: hsv2rgb(hue, (int)(f * 100), 100, r, g, b); break;
            case 2: r = g = b = 25 + (int)(f * 200); break;
            default: kelvin2rgb(2200 + (int)(f * 4300), r, g, b); break;
        }
        canvas.drawFastVLine(x + i, by, h, canvas.color565(r, g, b));
    }
    int mx = x + (int)(value * (w - 1));
    canvas.fillRect(mx - 1, by - 2, 3, h + 4, C_TEXT);
    if (sel) canvas.drawRect(x - 2, by - 3, w + 4, h + 6, C_ACCENT);
}

static void drawControl()
{
    drawHud(targetName.c_str(), lightOn ? "ON" : "OFF");

    // Muestra del color actual.
    uint8_t r, g, b;
    if (mode == Mode::White) kelvin2rgb(temp, r, g, b);
    else if (mode == Mode::Color) hsv2rgb(hue, sat, 100, r, g, b);
    else r = g = b = 200;
    int k = lightOn ? 25 + dim * 75 / 100 : 8;  // brillo aproximado de la muestra
    uint16_t swatch = canvas.color565(r * k / 100, g * k / 100, b * k / 100);
    canvas.fillRoundRect(10, 22, 56, 56, 8, swatch);
    canvas.drawRoundRect(10, 22, 56, 56, 8, lightOn ? C_DIM : C_OFF);
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(lightOn ? C_BG : C_DIM, swatch);
    if (mode == Mode::Scene) canvas.drawString(String(scene + 1), 38, 50);
    else if (!lightOn) canvas.drawString("OFF", 38, 50);

    const int x = 80, w = 150;
    if (mode == Mode::Color) {
        drawSlider(x, 20, w, 8, hue / 359.0f, field == 0, "TONO", String(hue) + "\xB0", 0);
        drawSlider(x, 46, w, 8, sat / 100.0f, field == 1, "SATURACION", String(sat) + "%", 1);
        drawSlider(x, 72, w, 8, (dim - 10) / 90.0f, field == 2, "BRILLO", String(dim) + "%", 2);
    } else if (mode == Mode::White) {
        drawSlider(x, 26, w, 8, (temp - 2200) / 4300.0f, field == 0, "TEMPERATURA", String(temp) + " K", 3);
        drawSlider(x, 58, w, 8, (dim - 10) / 90.0f, field == 1, "BRILLO", String(dim) + "%", 2);
    } else {
        canvas.setTextDatum(middle_left);
        for (int i = -2; i <= 2; i++) {
            int idx = (scene + i + 32) % 32;
            bool cur = i == 0;
            if (cur) canvas.setFont(&fonts::Font2);
            else canvas.setFont(&fonts::Font0);
            canvas.setTextColor(cur ? C_ACCENT : C_DIM, C_BG);
            canvas.drawString((cur ? "> " : "  ") + String(idx + 1) + " " + SCENES[idx], x, 47 + i * 13 + (cur ? 0 : (i < 0 ? -3 : 3)));
        }
        drawSlider(x, 86, w, 6, (dim - 10) / 90.0f, field == 1, "BRILLO", String(dim) + "%", 2);
    }

    // Pestañas de modo.
    static const char *TABS[3] = {"COLOR", "BLANCO", "ESCENA"};
    for (int i = 0; i < 3; i++) {
        bool on = (int)mode == i;
        int tx = 10 + i * 74;
        canvas.fillRoundRect(tx, 100, 70, 14, 4, on ? C_ACCENT : C_HUD);
        canvas.setFont(&fonts::Font0);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(on ? C_BG : C_DIM, on ? C_ACCENT : C_HUD);
        canvas.drawString(TABS[i], tx + 35, 107);
    }
    drawFooter(mode == Mode::Color ? "tab modo  space on/off  ;. campo  ,/ valor  1-8"
                                   : "tab modo  space on/off  ;. campo  ,/ valor");
}

static void render()
{
    canvas.fillScreen(C_BG);
    switch (screen) {
        case Screen::Connecting: drawConnecting(); break;
        case Screen::Networks: drawNetworks(); break;
        case Screen::Input: drawInput(); break;
        case Screen::Bulbs: drawBulbs(); break;
        case Screen::Control: drawControl(); break;
    }
    canvas.pushSprite(0, 0);
}

// --- Entrada ---------------------------------------------------------------------------------------------------
static bool hasChar(const Keyboard_Class::KeysState &ks, const char *chars)
{
    for (char c : ks.word) {
        if (strchr(chars, c)) return true;
    }
    return false;
}

static int fieldCount() { return mode == Mode::Color ? 3 : 2; }

// Cambia el valor del campo actual en `dir` pasos.
static void adjust(int dir, int mult)
{
    if (mode == Mode::Scene) {
        if (field == 0) scene = (scene + dir + 32) % 32;
        else dim = constrain(dim + dir * 3 * mult, 10, 100);
        if (field == 0) return;  // la escena se aplica con enter
    } else if (mode == Mode::Color) {
        if (field == 0) hue = (hue + dir * 3 * mult + 360) % 360;
        else if (field == 1) sat = constrain(sat + dir * 2 * mult, 0, 100);
        else dim = constrain(dim + dir * 2 * mult, 10, 100);
    } else {
        if (field == 0) temp = constrain(temp + dir * 50 * mult, 2200, 6500);
        else dim = constrain(dim + dir * 2 * mult, 10, 100);
    }
    lightOn = true;
    changed();
}

static void setMode(Mode m)
{
    mode = m;
    field = 0;
    lightOn = true;
    changed();
}

static void handleNetworks(const Keyboard_Class::KeysState &ks)
{
    if (hasChar(ks, "rR")) {
        notice = "";
        startScanNets();
        return;
    }
    if (nets.empty()) return;
    if (hasChar(ks, ";") && netSel > 0) netSel--;
    if (hasChar(ks, ".") && netSel < (int)nets.size() - 1) netSel++;
    if (ks.enter) {
        ssid = nets[netSel].ssid;
        pass = "";
        notice = "";
        if (nets[netSel].open) {
            prefs.putString("ssid", ssid);
            prefs.putString("pass", pass);
            connectWifi();
        } else {
            beginInput(InputFor::Password, "Clave de " + ssid);
        }
    }
}

static void handleInput(const Keyboard_Class::KeysState &ks)
{
    if (ks.tab) {
        screen = inputFor == InputFor::Password ? Screen::Networks : Screen::Bulbs;
        notice = "";
        return;
    }
    if (ks.del) {
        if (inputBuf.length()) inputBuf.remove(inputBuf.length() - 1);
        return;
    }
    if (ks.enter) {
        if (inputFor == InputFor::Password) {
            pass = inputBuf;
            prefs.putString("ssid", ssid);
            prefs.putString("pass", pass);
            connectWifi();
        } else {
            IPAddress ip;
            if (!ip.fromString(inputBuf)) {
                notice = "IP invalida (ej. 192.168.1.50)";
                return;
            }
            prefs.putString("lastip", inputBuf);
            screen = Screen::Bulbs;
            discovering = false;
            addBulb(ip, "", -60);
            bulbSel = bulbs.size() > 1 ? bulbs.size() : 0;
            notice = "";
        }
        return;
    }
    for (char c : ks.word) {
        if (inputBuf.length() < 63) inputBuf += c;
    }
}

static void handleBulbs(const Keyboard_Class::KeysState &ks)
{
    if (hasChar(ks, "wW") || ks.del) {
        goNetworks();
        return;
    }
    if (hasChar(ks, "rR")) {
        startDiscovery();
        return;
    }
    if (hasChar(ks, "iI")) {
        notice = "";
        beginInput(InputFor::Ip, "Direccion IP de la lampara");
        inputBuf = prefs.getString("lastip", "");
        return;
    }
    int rows = bulbs.size() + (bulbs.size() > 1 ? 1 : 0);
    if (!rows) return;
    if (hasChar(ks, ";") && bulbSel > 0) bulbSel--;
    if (hasChar(ks, ".") && bulbSel < rows - 1) bulbSel++;
    if (ks.enter) {
        if (bulbs.size() > 1 && bulbSel == 0) {
            std::vector<IPAddress> all;
            for (auto &b : bulbs) all.push_back(b.ip);
            openControl(all, "Todas (" + String(bulbs.size()) + ")");
        } else {
            const Bulb &b = bulbs[bulbSel - (bulbs.size() > 1 ? 1 : 0)];
            openControl({b.ip}, bulbLabel(b).substring(4));
        }
    }
}

static void handleControl(const Keyboard_Class::KeysState &ks)
{
    if (ks.del) {
        screen = Screen::Bulbs;
        heldDir = 0;
        startDiscovery();
        return;
    }
    if (ks.tab) {
        setMode((Mode)(((int)mode + 1) % 3));
        return;
    }
    if (ks.space) {
        lightOn = !lightOn;
        changed();
        return;
    }
    if (ks.enter) {
        lightOn = true;
        changed();
        return;
    }
    if (hasChar(ks, ";") && field > 0) field--;
    else if (hasChar(ks, ".") && field < fieldCount() - 1) field++;
    else if (hasChar(ks, ",")) {
        heldDir = -1;
        heldSince = heldLast = millis();
        adjust(-1, 1);
    } else if (hasChar(ks, "/")) {
        heldDir = 1;
        heldSince = heldLast = millis();
        adjust(1, 1);
    } else if (mode == Mode::Color) {
        for (char c : ks.word) {
            if (c >= '1' && c <= '8') {
                hue = PRESETS[c - '1'].hue;
                sat = 100;
                lightOn = true;
                changed();
            }
        }
    }
    dirty = true;
}

static void handleKeys()
{
    M5Cardputer.update();
    bool down = M5Cardputer.Keyboard.isPressed();
    if (M5Cardputer.Keyboard.isChange() && down) {
        auto ks = M5Cardputer.Keyboard.keysState();
        dirty = true;
        switch (screen) {
            case Screen::Connecting:
                if (ks.del) goNetworks();
                break;
            case Screen::Networks: handleNetworks(ks); break;
            case Screen::Input: handleInput(ks); break;
            case Screen::Bulbs: handleBulbs(ks); break;
            case Screen::Control: handleControl(ks); break;
        }
    }
    if (screen == Screen::Control && heldDir) {
        auto ks = M5Cardputer.Keyboard.keysState();
        if (!down || !hasChar(ks, heldDir < 0 ? "," : "/")) {
            heldDir = 0;
        } else {
            uint32_t now = millis();
            if (now - heldSince > 300 && now - heldLast >= 60) {
                heldLast = now;
                adjust(heldDir, now - heldSince > 1200 ? 3 : 1);
            }
        }
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
    C_HUD = canvas.color565(30, 34, 42);
    C_TEXT = canvas.color565(225, 225, 230);
    C_DIM = canvas.color565(120, 120, 130);
    C_ACCENT = canvas.color565(255, 196, 64);
    C_RED = canvas.color565(235, 70, 60);
    C_ON = canvas.color565(80, 210, 110);
    C_OFF = canvas.color565(50, 50, 58);

    prefs.begin("wiz-rc", false);
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    udp.begin(WIZ_PORT);

    if (ssid.length()) connectWifi();
    else goNetworks();
    render();
}

void loop()
{
    handleKeys();
    uint32_t now = millis();

    switch (screen) {
        case Screen::Connecting:
            if (WiFi.status() == WL_CONNECTED) {
                screen = Screen::Bulbs;
                startDiscovery();
            } else if (now - connectStart > CONNECT_MS) {
                notice = "No pude conectar a " + ssid;
                goNetworks();
            }
            dirty = true;  // barra de progreso
            break;
        case Screen::Networks:
            if (netScanning && WiFi.scanComplete() >= 0) collectNets();
            break;
        case Screen::Input:
            dirty = true;  // cursor parpadeante
            break;
        case Screen::Bulbs:
            if (discovering) {
                if (now - discoverStart > DISCOVER_MS) {
                    discovering = false;
                    dirty = true;
                } else if (now - lastProbe > 700) {
                    probe();
                }
            }
            break;
        case Screen::Control:
            if (WiFi.status() != WL_CONNECTED) {
                notice = "Se perdio el WiFi";
                connectWifi();
            } else if (pending && now - lastSendMs >= SEND_MIN_MS) {
                sendState();
            }
            break;
    }
    pollUdp();

    if (dirty) {
        render();
        dirty = false;
    }
    delay(2);
}
