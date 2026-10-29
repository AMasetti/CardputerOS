// BB-8 RC — control remoto del BB-8 original de Sphero (protocolo v1; Ollie usa el mismo) por BLE.
//
// Teclas (las flechas impresas en el teclado):
//   ;  .          adelante / atrás (mientras las mantenés apretadas)
//   ,  /          girar el rumbo a izquierda / derecha
//   -  =          velocidad menos / más
//   1..8          color del LED        c  calibrar: prende la luz trasera, girá con , / hasta que
//                                       apunte hacia vos y apretá enter (ese rumbo pasa a ser "adelante")
//   s             dormir al robot      space  frenar
// Soltar las teclas de manejo frena al robot. Para salir: Fn + `. Se desconecta de forma limpia.

#include <M5Cardputer.h>
#include <string>
#include <vector>

#include "sphero_core.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HUD_H = 15;
static constexpr uint32_t ROLL_MS = 100;

static M5Canvas canvas(&M5Cardputer.Display);
static uint16_t C_BG, C_HUD, C_TEXT, C_DIM, C_ACCENT, C_OK, C_ERR;

enum class Screen { Scan, Connecting, Drive };
static Screen screen = Screen::Scan;
static bool dirty = true;
static std::string status;

struct Robot { std::string name; uint8_t addr[6]; uint8_t type; int8_t rssi; };
static std::vector<Robot> robots;
static int selected = 0;
static bool scanning = false;
static bool demo = false;

static sphero::Builder pb;
static uint16_t hCmd = 0;
static int speedPct = 50, heading = 0;
static bool calibrating = false, moving = false;
static uint8_t color[3] = {0, 120, 255};

static const uint8_t PALETTE[8][3] = {{255, 0, 0}, {255, 120, 0}, {255, 220, 0}, {0, 220, 60},
                                      {0, 200, 220}, {0, 80, 255}, {170, 0, 255}, {255, 255, 255}};

// --- BLE ------------------------------------------------------------------------------------
static bool writeCmd(const uint8_t *p, size_t n)
{
    if (demo) return true;
    return cpos_api->ble.write(hCmd, p, n, 1) == 0;
}

static void rgb(const uint8_t c[3])
{
    uint8_t p[16];
    color[0] = c[0]; color[1] = c[1]; color[2] = c[2];
    writeCmd(p, pb.rgb(p, c[0], c[1], c[2]));
}

static void roll(int speed)
{
    uint8_t p[16];
    writeCmd(p, pb.roll(p, speed, heading));
}

static std::string advName(const cpos_ble_adv_t &a)
{
    for (int i = 0; i + 1 < a.len;) {
        int l = a.data[i];
        if (l == 0 || i + 1 + l > a.len) break;
        if (a.data[i + 1] == 0x09 || a.data[i + 1] == 0x08) return std::string((const char *)&a.data[i + 2], l - 1);
        i += l + 1;
    }
    return "";
}

static void startScan()
{
    robots.clear();
    selected = 0;
    cpos_api->ble.begin();
    scanning = cpos_api->ble.scan_start(8000, 1, 100, 80, 1) == 0 || cpos_api->ble.scanning();
    dirty = true;
}

static void pollScan()
{
    cpos_ble_adv_t a;
    while (cpos_api->ble.scan_read(&a)) {
        std::string n = advName(a);
        if (n.compare(0, 3, "BB-") != 0 && n.compare(0, 3, "2B-") != 0) continue;  // BB-8 v1 y Ollie
        bool known = false;
        for (auto &r : robots) if (memcmp(r.addr, a.addr, 6) == 0) { r.rssi = a.rssi; known = true; }
        if (known) continue;
        Robot r;
        r.name = n;
        memcpy(r.addr, a.addr, 6);
        r.type = a.addr_type;
        r.rssi = a.rssi;
        robots.push_back(r);
    }
    if (scanning && !cpos_api->ble.scanning()) scanning = false;
    dirty = true;
}

static bool findChar(uint8_t chr, uint16_t &handle)
{
    static cpos_ble_attr_t svc[12], ch[10];
    int ns = cpos_api->ble.services(svc, 12);
    for (int s = 0; s < ns; s++) {
        int nc = cpos_api->ble.characteristics(s, ch, 10);
        for (int c = 0; c < nc; c++)
            if (sphero::uuid_is(ch[c].uuid, chr)) { handle = ch[c].handle; return true; }
    }
    return false;
}

static bool connectRobot(const Robot &r)
{
    status.clear();
    if (cpos_api->ble.scanning()) cpos_api->ble.scan_stop();
    if (cpos_api->ble.connect(r.addr, r.type, 10000) != 0) { status = "No pude conectar"; return false; }
    uint16_t hDos, hTx, hWake;
    if (!findChar(sphero::CHR_ANTIDOS, hDos) || !findChar(sphero::CHR_TXPOWER, hTx) ||
        !findChar(sphero::CHR_WAKE, hWake) || !findChar(sphero::CHR_CMD, hCmd)) {
        cpos_api->ble.disconnect();
        status = "No es un Sphero v1";
        return false;
    }
    bool ok = cpos_api->ble.write(hDos, sphero::WAKE_ANTIDOS, sizeof(sphero::WAKE_ANTIDOS), 1) == 0 &&
              cpos_api->ble.write(hTx, sphero::WAKE_TXPOWER, sizeof(sphero::WAKE_TXPOWER), 1) == 0 &&
              cpos_api->ble.write(hWake, sphero::WAKE_CPU, sizeof(sphero::WAKE_CPU), 1) == 0;
    if (!ok) { cpos_api->ble.disconnect(); status = "El robot no despertó"; return false; }
    pb.seq = 0;
    heading = 0;
    calibrating = false;
    rgb(color);
    return true;
}

static void disconnectRobot()
{
    if (!demo && cpos_api->ble.connected()) {
        roll(0);
        delay(50);
        cpos_api->ble.disconnect();  // si no se corta bien, el robot rechaza la próxima conexión
    }
}

// --- dibujo ---------------------------------------------------------------------------------
static void drawHud(const char *left, const char *right)
{
    canvas.fillRect(0, 0, SCREEN_W, HUD_H, C_HUD);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_ACCENT, C_HUD);
    canvas.setTextDatum(middle_left);
    canvas.drawString(left, 4, HUD_H / 2);
    canvas.setTextColor(C_DIM, C_HUD);
    canvas.setTextDatum(middle_right);
    canvas.drawString(right, SCREEN_W - 4, HUD_H / 2);
}

static void renderScan()
{
    canvas.fillScreen(C_BG);
    drawHud("BB-8 RC", "v" APP_VERSION);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::lgfxJapanGothicP_12);
    if (robots.empty()) {
        canvas.setTextColor(C_TEXT, C_BG);
        canvas.drawString(scanning ? "Buscando BB-8..." : "No encontré ningún BB-8", 10, 30);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("Sacalo del cargador y sacudilo", 10, 50);
    }
    for (int i = 0; i < (int)robots.size() && i < 6; i++) {
        int y = 24 + i * 18;
        if (i == selected) canvas.fillRoundRect(4, y - 2, SCREEN_W - 8, 17, 4, C_HUD);
        canvas.setTextColor(i == selected ? C_ACCENT : C_TEXT, i == selected ? C_HUD : C_BG);
        canvas.drawString(robots[i].name.c_str(), 12, y);
        char r[16];
        snprintf(r, sizeof(r), "%d dBm", robots[i].rssi);
        canvas.setTextDatum(top_right);
        canvas.setTextColor(C_DIM, i == selected ? C_HUD : C_BG);
        canvas.drawString(r, SCREEN_W - 10, y);
        canvas.setTextDatum(top_left);
    }
    if (!status.empty()) {
        canvas.setTextColor(C_ERR, C_BG);
        canvas.drawString(status.c_str(), 10, 104);
    }
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("enter conectar  r buscar  d demo", 10, 120);
    canvas.pushSprite(0, 0);
}

static void renderDrive()
{
    canvas.fillScreen(C_BG);
    drawHud(demo ? "BB-8 RC  demo" : robots[selected].name.c_str(), "Fn+` salir");
    // brújula del rumbo
    const int cx = 60, cy = 75, R = 44;
    canvas.drawCircle(cx, cy, R, C_DIM);
    canvas.fillCircle(cx, cy, 10, canvas.color565(color[0], color[1], color[2]));
    float a = heading * 3.14159265f / 180.0f;
    int ex = cx + (int)(sinf(a) * (R - 4)), ey = cy - (int)(cosf(a) * (R - 4));
    canvas.drawLine(cx, cy, ex, ey, calibrating ? C_ERR : C_ACCENT);
    canvas.fillCircle(ex, ey, 3, calibrating ? C_ERR : C_ACCENT);
    canvas.setFont(&fonts::lgfxJapanGothicP_12);
    canvas.setTextDatum(top_left);
    char b[32];
    snprintf(b, sizeof(b), "Rumbo %d", heading);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(b, 130, HUD_H + 8);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("Vel", 130, HUD_H + 28);
    canvas.drawRoundRect(154, HUD_H + 30, 76, 8, 3, C_DIM);
    canvas.fillRoundRect(155, HUD_H + 31, 74 * speedPct / 100, 6, 2, C_ACCENT);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(calibrating ? C_ERR : (moving ? C_OK : C_DIM), C_BG);
    canvas.drawString(calibrating ? "CALIBRANDO: enter ok" : (moving ? "en marcha" : "frenado"), 130, HUD_H + 48);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(";. mover  ,/ girar", 130, HUD_H + 68);
    canvas.drawString("-= vel  1-8 color", 130, HUD_H + 80);
    canvas.drawString("c calibrar  s dormir", 130, HUD_H + 92);
    if (!demo && !cpos_api->ble.connected()) {
        canvas.fillRoundRect(40, 50, 160, 30, 4, C_ERR);
        canvas.setTextColor(C_TEXT, C_ERR);
        canvas.setTextDatum(middle_center);
        canvas.drawString("SIN CONEXION", 120, 65);
    }
    canvas.pushSprite(0, 0);
}

// --- manejo ---------------------------------------------------------------------------------
static uint32_t lastRoll = 0;

static void handleKeys()
{
    M5Cardputer.update();
    auto ks = M5Cardputer.Keyboard.keysState();
    bool fwd = false, back = false, left = false, right = false;
    for (char c : ks.word) {
        if (c == ';') fwd = true;
        else if (c == '.') back = true;
        else if (c == ',') left = true;
        else if (c == '/') right = true;
    }
    if (millis() - lastRoll >= ROLL_MS) {
        lastRoll = millis();
        int turn = (right ? 1 : 0) - (left ? 1 : 0);
        if (turn) { heading = (heading + turn * 12 + 360) % 360; dirty = true; }
        int spd = 255 * speedPct / 100;
        bool active = !calibrating && (fwd || back);
        if (active != moving) { moving = active; dirty = true; }
        if (calibrating) roll(0);  // girar sin avanzar: el robot orienta la luz trasera
        else if (fwd) roll(spd);
        else if (back) { int h = heading; heading = (heading + 180) % 360; roll(spd); heading = h; }
        else if (turn) roll(0);
        else roll(0);  // sin teclas: velocidad 0 = frena (y mantiene vivo el enlace)
    }

    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    uint8_t p[16];
    if (calibrating && ks.enter) {
        writeCmd(p, pb.set_heading(p, 0));
        writeCmd(p, pb.back_led(p, 0));
        heading = 0;
        calibrating = false;
        dirty = true;
        return;
    }
    for (char c : ks.word) {
        if (c == '-') { speedPct = speedPct > 10 ? speedPct - 10 : 10; dirty = true; }
        else if (c == '=' || c == '+') { speedPct = speedPct < 100 ? speedPct + 10 : 100; dirty = true; }
        else if (c >= '1' && c <= '8') { rgb(PALETTE[c - '1']); dirty = true; }
        else if (c == 'c') {
            calibrating = !calibrating;
            writeCmd(p, pb.back_led(p, calibrating ? 255 : 0));
            if (!calibrating) heading = 0;
            dirty = true;
        } else if (c == 's') {
            writeCmd(p, pb.sleep(p));
            disconnectRobot();
            demo = false;
            screen = Screen::Scan;
            startScan();
            dirty = true;
            return;
        } else if (c == ' ') { roll(0); }
    }
}

// --- Arduino --------------------------------------------------------------------------------
void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    canvas.setColorDepth(16);
    canvas.createSprite(SCREEN_W, SCREEN_H);
    C_BG = canvas.color565(10, 12, 18);
    C_HUD = canvas.color565(26, 30, 44);
    C_TEXT = canvas.color565(225, 228, 238);
    C_DIM = canvas.color565(120, 126, 145);
    C_ACCENT = canvas.color565(255, 140, 40);
    C_OK = canvas.color565(80, 220, 120);
    C_ERR = canvas.color565(230, 60, 60);
    startScan();
}

void loop()
{
    switch (screen) {
        case Screen::Scan: {
            M5Cardputer.update();
            pollScan();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto ks = M5Cardputer.Keyboard.keysState();
                if (ks.enter && !robots.empty()) { status.clear(); screen = Screen::Connecting; break; }
                for (char c : ks.word) {
                    if (c == ';' && selected > 0) { selected--; dirty = true; }
                    else if (c == '.' && selected + 1 < (int)robots.size()) { selected++; dirty = true; }
                    else if (c == 'r' || c == 'R') { status.clear(); startScan(); }
                    else if (c == 'd' || c == 'D') { demo = true; speedPct = 50; screen = Screen::Drive; dirty = true; }
                }
            }
            if (dirty) { renderScan(); dirty = false; }
            delay(10);
            break;
        }
        case Screen::Connecting:
            canvas.fillScreen(C_BG);
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(C_TEXT, C_BG);
            canvas.setTextDatum(middle_center);
            canvas.drawString("Conectando...", SCREEN_W / 2, 60);
            canvas.pushSprite(0, 0);
            if (connectRobot(robots[selected])) screen = Screen::Drive;
            else { screen = Screen::Scan; startScan(); }
            dirty = true;
            break;
        case Screen::Drive: {
            handleKeys();
            static uint32_t lastDraw = 0;
            if ((dirty && millis() - lastDraw > 40) || millis() - lastDraw > 500) {
                renderDrive();
                dirty = false;
                lastDraw = millis();
            }
            delay(1);
            break;
        }
    }
}
