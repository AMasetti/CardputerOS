// Robot Control — un solo control remoto para todos los robots y drones de juguete:
//   Sphero BB-8 / Ollie (v1), R2-D2 / R2-Q5 / BB-9E / BOLT (v2)   — Bluetooth LE
//   Parrot Rolling Spider                                          — Bluetooth LE
//   Parrot Jumping Sumo (con video)                                — WiFi
// La pantalla de inicio busca por BLE y WiFi a la vez y lista lo que encuentra. Para salir: Fn + `.

#include <M5Cardputer.h>
#include <WiFi.h>
#include <vector>

#include "robot.h"
#include "sphero1_core.h"
#include "sphero2_core.h"
#include "minidrone_core.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

M5Canvas canvas(&M5Cardputer.Display);
uint16_t C_BG, C_HUD, C_TEXT, C_DIM, C_ACCENT, C_OK, C_WARN, C_ERR, C_VID;

enum class Screen { Scan, Connecting, Drive };
static Screen screen = Screen::Scan;
static bool dirty = true;
static std::string status;

static std::vector<Found> found;
static int selected = 0;
static bool bleScanning = false, wifiScanning = false;
static uint32_t scanStart = 0;
static Robot *robot = nullptr;

void drawHud(const char *left, const char *right)
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

// --- escaneo ---------------------------------------------------------------------------------
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
    found.clear();
    selected = 0;
    cpos_api->ble.begin();
    bleScanning = cpos_api->ble.scan_start(10000, 1, 100, 80, 1) == 0 || cpos_api->ble.scanning();
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    wifiScanning = true;
    scanStart = millis();
    dirty = true;
}

static void addFound(const Found &f)
{
    for (auto &x : found)
        if (x.fam == f.fam && x.name == f.name) { x.rssi = f.rssi; return; }
    found.push_back(f);
    dirty = true;
}

static void pollScan()
{
    cpos_ble_adv_t a;
    while (cpos_api->ble.scan_read(&a)) {
        std::string n = advName(a);
        Found f{};
        f.name = n;
        memcpy(f.addr, a.addr, 6);
        f.type = a.addr_type;
        f.rssi = a.rssi;
        int k;
        if (n.compare(0, 3, "BB-") == 0 || n.compare(0, 3, "2B-") == 0) f.fam = F_SPHERO1;
        else if ((k = sphero2::kind_of(n.c_str())) >= 0) { f.fam = F_SPHERO2; f.sub = k; }
        else if (mini::is_minidrone(n.c_str())) f.fam = F_MINI;
        else continue;
        addFound(f);
    }
    if (bleScanning && !cpos_api->ble.scanning()) { bleScanning = false; dirty = true; }
    if (wifiScanning) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            for (int i = 0; i < n; i++) {
                std::string name = WiFi.SSID(i).c_str();
                if (name.find("Sumo") == std::string::npos && name.find("sumo") == std::string::npos) continue;
                Found f{};
                f.fam = F_SUMO;
                f.name = name;
                f.rssi = WiFi.RSSI(i);
                addFound(f);
            }
            WiFi.scanDelete();
            wifiScanning = false;
            dirty = true;
        }
    }
}

static const char *famName(Family f)
{
    switch (f) {
        case F_SUMO: return "Sumo";
        case F_SPHERO1: return "Sphero";
        case F_SPHERO2: return "Sphero";
        case F_MINI: return "Parrot";
    }
    return "";
}

static void renderScan()
{
    canvas.fillScreen(C_BG);
    drawHud("ROBOT CONTROL", status.empty() ? "v" APP_VERSION : status.c_str());
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::lgfxJapanGothicP_12);
    if (found.empty()) {
        canvas.setTextColor(C_TEXT, C_BG);
        canvas.drawString(bleScanning || wifiScanning ? "Buscando robots..." : "No encontré ningún robot", 10, 30);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("BB-8 R2-D2 BB-9E BOLT Spider Sumo", 10, 50);
    }
    int first = selected > 4 ? selected - 4 : 0;
    for (int i = first; i < (int)found.size() && i < first + 5; i++) {
        int y = 24 + (i - first) * 18;
        if (i == selected) canvas.fillRoundRect(4, y - 2, SCREEN_W - 8, 17, 4, C_HUD);
        canvas.setTextColor(i == selected ? C_ACCENT : C_TEXT, i == selected ? C_HUD : C_BG);
        canvas.drawString(found[i].name.c_str(), 12, y);
        char r[24];
        snprintf(r, sizeof(r), "%s %d", famName(found[i].fam), found[i].rssi);
        canvas.setTextDatum(top_right);
        canvas.setTextColor(C_DIM, i == selected ? C_HUD : C_BG);
        canvas.drawString(r, SCREEN_W - 10, y);
        canvas.setTextDatum(top_left);
    }
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("enter conectar   r buscar", 10, 120);
    canvas.pushSprite(0, 0);
}

// --- manejo ----------------------------------------------------------------------------------
static Robot *makeRobot(const Found &f)
{
    switch (f.fam) {
        case F_SUMO: return make_sumo(f);
        case F_SPHERO1: return make_sphero1(f);
        case F_SPHERO2: return make_sphero2(f);
        case F_MINI: return make_mini(f);
    }
    return nullptr;
}

static void closeRobot()
{
    if (!robot) return;
    robot->close();
    delete robot;
    robot = nullptr;
}

static void handleDrive()
{
    M5Cardputer.update();
    auto ks = M5Cardputer.Keyboard.keysState();
    std::string word(ks.word.begin(), ks.word.end());
    Held h{};
    h.word = &word;
    h.fwd = h.has(';'); h.back = h.has('.'); h.left = h.has(','); h.right = h.has('/');
    robot->tick(h);
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (ks.enter) robot->enter();
        if (ks.space) robot->key(' ');
        for (char c : word) robot->key(c);
        if (ks.del) {  // backspace vuelve a la lista
            closeRobot();
            screen = Screen::Scan;
            startScan();
            dirty = true;
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
    C_BG = canvas.color565(10, 12, 18);
    C_HUD = canvas.color565(26, 30, 44);
    C_TEXT = canvas.color565(225, 228, 238);
    C_DIM = canvas.color565(120, 126, 145);
    C_ACCENT = canvas.color565(255, 140, 40);
    C_OK = canvas.color565(80, 220, 120);
    C_WARN = canvas.color565(255, 200, 60);
    C_ERR = canvas.color565(230, 60, 60);
    C_VID = canvas.color565(4, 6, 10);
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
                if (ks.enter && !found.empty()) { status.clear(); screen = Screen::Connecting; break; }
                for (char c : ks.word) {
                    if (c == ';' && selected > 0) { selected--; dirty = true; }
                    else if (c == '.' && selected + 1 < (int)found.size()) { selected++; dirty = true; }
                    else if (c == 'r' || c == 'R') { status.clear(); startScan(); }
                }
            }
            if (dirty) { renderScan(); dirty = false; }
            delay(10);
            break;
        }
        case Screen::Connecting: {
            const Found f = found[selected];
            canvas.fillScreen(C_BG);
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(C_TEXT, C_BG);
            canvas.setTextDatum(middle_center);
            canvas.drawString("Conectando...", SCREEN_W / 2, 50);
            canvas.setFont(&fonts::Font0);
            canvas.setTextColor(C_DIM, C_BG);
            canvas.drawString(f.name.c_str(), SCREEN_W / 2, 75);
            canvas.pushSprite(0, 0);
            if (cpos_api->ble.scanning()) cpos_api->ble.scan_stop();
            robot = makeRobot(f);
            if (robot && robot->connect(status)) { screen = Screen::Drive; }
            else { closeRobot(); screen = Screen::Scan; startScan(); }
            dirty = true;
            break;
        }
        case Screen::Drive: {
            handleDrive();
            if (!robot) break;
            static uint32_t lastDraw = 0;
            if ((robot->dirty && millis() - lastDraw > 40) || millis() - lastDraw > 500) {
                drawHud(robot->title(), robot->hint());
                robot->draw();
                canvas.pushSprite(0, 0);
                robot->dirty = false;
                robot->fresh = false;
                lastDraw = millis();
            }
            delay(1);
            break;
        }
    }
}

// --- Intents (docs/intents.md) ----------------------------------------------------------------
// Cada intent es una sesión corta: escanea, se conecta al robot, hace una cosa y se desconecta.
static bool scanRobots(int secs)
{
    found.clear();
    cpos_api->ble.begin();
    if (cpos_api->ble.scan_start(secs * 1000, 1, 100, 80, 1) != 0 && !cpos_api->ble.scanning()) return false;
    for (uint32_t t0 = millis(); millis() - t0 < (uint32_t)secs * 1000; delay(20)) pollScan();
    cpos_api->ble.scan_stop();
    return true;
}

void on_intent(const Intent &in, IntentResult &out)
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, false);
    if (in.is("robot.scan")) {
        int secs = in.i("seconds", 4);
        if (secs < 2 || secs > 15) { out.error(CPOS_INTENT_BAD_ARG); return; }
        if (!scanRobots(secs)) { out.error(CPOS_INTENT_FAILED); return; }
        for (auto &f : found) if (f.fam != F_SUMO) out.item(f.name.c_str());
        out.done();
        return;
    }
    if (!in.is("robot.color") && !in.is("robot.drive") && !in.is("robot.sound")) { out.error(CPOS_INTENT_NO_APP); return; }
    char want[24] = "";
    in.get("name", want, sizeof(want));  // sin name: el robot Sphero con mejor señal
    if (!scanRobots(5)) { out.error(CPOS_INTENT_FAILED); return; }
    const Found *pick = nullptr;
    for (auto &f : found) {
        if (f.fam != F_SPHERO1 && f.fam != F_SPHERO2) continue;
        if (want[0] ? f.name == want : (!pick || f.rssi > pick->rssi)) { pick = &f; if (want[0]) break; }
    }
    if (!pick) { out.error(CPOS_INTENT_FAILED); return; }
    Robot *r = makeRobot(*pick);
    std::string status;
    if (!r || !r->connect(status)) { delete r; out.error(CPOS_INTENT_FAILED); return; }
    int rc = r->intent(in);
    r->close();  // desconexión limpia: si no, el robot rechaza la próxima conexión
    delete r;
    if (rc != CPOS_INTENT_OK) out.error(rc);
    else out.ok();
}
