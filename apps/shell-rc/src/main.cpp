// Control de autos Shell Motorsport por BLE para M5Stack Cardputer / Cardputer-Adv
//
// Soporta las dos generaciones, con el protocolo de:
//   2024 (QCAR-*): github.com/AMasetti/shell-motorsport-rc-lib
//     16 bytes cifrados con AES-128 ECB: [0, 'C','T','L', adelante, atrás,
//     izquierda, derecha, 0, velocidad, 0...]
//   2025 (SL-*):   github.com/AMasetti/shell-motorsport-rc-lib-2025
//     8 bytes en claro: [modo, adelante, atrás, izquierda, derecha, luces,
//     turbo, donut], más batería por el servicio estándar 0x180F.
//
// Teclas:
//   elegir generación   ; .  o  1 2, enter
//   lista de autos      ; .  elegir, enter conectar, r buscar otra vez, del volver
//   manejo              ; . , /  adelante / atrás / izquierda / derecha (también w s a d)
//                       2024: 1-4 velocidad
//                       2025: l luces, t turbo, o donut, m modo 1/2
//                       del   desconectar

#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <mbedtls/aes.h>

#include <algorithm>
#include <mutex>
#include <vector>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HUD_H = 15;

static constexpr uint32_t SCAN_SECONDS = 6;
static constexpr uint32_t SEND_MS = 30;  // los autos esperan un flujo continuo de comandos
static constexpr int MAX_CARS = 8;

// --- Protocolos ----------------------------------------------------------------
enum Gen : uint8_t { GEN_2024 = 0, GEN_2025 = 1 };

struct GenInfo {
    const char *label;
    const char *prefix;  // prefijo del nombre BLE
    uint16_t service;
    const char *writeChar;
};

static const GenInfo GENS[2] = {
    {"2024", "QCAR", 0xFFF0, "d44bc439-abfd-45a2-b575-925416129600"},
    {"2025", "SL-", 0xFFF0, "0000fff1-0000-1000-8000-00805f9b34fb"},
};

static const uint8_t AES_KEY_2024[16] = {0x34, 0x52, 0x2a, 0x5b, 0x7a, 0x6e, 0x49, 0x2c,
                                         0x08, 0x09, 0x0a, 0x9d, 0x8d, 0x2a, 0x23, 0xf8};
// Mensaje de parada que usa la librería de 2024 en stop().
static const uint8_t IDLE_2024[16] = {0xe1, 0x05, 0x5f, 0x54, 0xd8, 0x80, 0xf4, 0x9c,
                                      0x2c, 0xe5, 0x47, 0x26, 0x7f, 0x93, 0x0b, 0xf2};
static const uint8_t SPEEDS_2024[4] = {0x16, 0x32, 0x48, 0x64};

static const NimBLEUUID BATTERY_SERVICE((uint16_t)0x180F);
static const NimBLEUUID BATTERY_CHAR((uint16_t)0x2A19);

// --- Estado ----------------------------------------------------------------------
enum class Screen { PickGen, List, Driving };

struct Car {
    std::string name;
    NimBLEAddress addr;
    int rssi;
};

struct Controls {
    bool fwd, back, left, right;
};

static M5Canvas canvas(&M5Cardputer.Display);
static Preferences prefs;

static Screen screen = Screen::PickGen;
static Gen gen = GEN_2025;
static bool dirty = true;

static std::vector<Car> cars;
static int selected;
static bool scanning;
static uint32_t lastScanPoll;
static String notice;  // mensaje corto en la lista (error de conexión, etc.)

static NimBLEClient *client;
static NimBLERemoteCharacteristic *writeChr;
static std::string carName;
static Controls ctl;
static int speedIdx = 2;  // 2024
static bool lights, turbo, donut;  // 2025
static uint8_t mode = 1;            // 2025
static volatile int battery = -1;   // 2025, -1 = desconocida
static volatile bool batteryChanged;
static uint32_t lastSend;
static uint32_t packets;

static uint16_t C_BG, C_HUD, C_TEXT, C_DIM, C_ACCENT, C_RED, C_ON, C_OFF, C_YELLOW;

// --- BLE -------------------------------------------------------------------------
// El escaneo corre en la tarea de NimBLE: los autos encontrados se guardan acá
// bajo un lock y el loop los copia a `cars`.
static std::mutex foundLock;
static Car found[MAX_CARS];
static int foundCount;
static volatile bool foundChanged;

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *d) override
    {
        if (!d->haveName()) return;
        std::string name = d->getName();
        if (name.find(GENS[gen].prefix) == std::string::npos) return;
        NimBLEAddress addr = d->getAddress();
        int rssi = d->getRSSI();

        std::lock_guard<std::mutex> lock(foundLock);
        int i = 0;
        while (i < foundCount && !(found[i].addr == addr)) i++;
        if (i < MAX_CARS) {
            if (i == foundCount) foundCount++;
            found[i].name = name;
            found[i].addr = addr;
            found[i].rssi = rssi;
            foundChanged = true;
        }
    }
};
static ScanCallbacks scanCallbacks;

static void startScan()
{
    {
        std::lock_guard<std::mutex> lock(foundLock);
        foundCount = 0;
    }
    cars.clear();
    selected = 0;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->stop();
    scan->clearResults();
    scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);  // duplicados: así se actualiza la señal
    scan->setMaxResults(0);   // no guardar resultados en la librería, alcanza con el callback
    scan->setActiveScan(true);  // el nombre suele venir en la respuesta al escaneo
    scan->setInterval(100);
    scan->setWindow(99);
    scanning = scan->start(SCAN_SECONDS, [](NimBLEScanResults) {}, false);
    dirty = true;
}

static void stopScan()
{
    NimBLEDevice::getScan()->stop();
    scanning = false;
}

// Copia los autos encontrados, ordenados por señal, manteniendo la selección.
static void collectCars()
{
    std::string prevSel = selected < (int)cars.size() ? cars[selected].addr.toString() : "";
    std::vector<Car> copy;
    {
        std::lock_guard<std::mutex> lock(foundLock);
        copy.assign(found, found + foundCount);
        foundChanged = false;
    }

    std::sort(copy.begin(), copy.end(), [](const Car &a, const Car &b) { return a.rssi > b.rssi; });
    cars.swap(copy);
    selected = 0;
    for (int i = 0; i < (int)cars.size(); i++) {
        if (cars[i].addr.toString() == prevSel) selected = i;
    }
}

static void onBattery(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
    if (len >= 1) {
        battery = data[0];
        batteryChanged = true;
    }
}

static NimBLERemoteCharacteristic *findWriteChar()
{
    NimBLEUUID uuid(GENS[gen].writeChar);
    NimBLERemoteService *svc = client->getService(NimBLEUUID(GENS[gen].service));
    if (svc) {
        if (auto *c = svc->getCharacteristic(uuid)) return c;
    }
    // Por si el firmware la publica en otro servicio.
    for (auto *s : *client->getServices(true)) {
        if (auto *c = s->getCharacteristic(uuid)) return c;
    }
    return nullptr;
}

static void drawMessage(const char *title, const char *line);

static bool connectTo(const Car &car)
{
    stopScan();
    drawMessage("Conectando", car.name.c_str());

    if (!client) {
        client = NimBLEDevice::createClient();
        client->setConnectTimeout(8);
    }
    if (!client->connect(car.addr)) {
        notice = "No pude conectar";
        return false;
    }

    writeChr = findWriteChar();
    if (!writeChr) {
        client->disconnect();
        notice = "No es un auto " + String(GENS[gen].label) + "?";
        return false;
    }

    battery = -1;
    if (gen == GEN_2025) {
        if (auto *svc = client->getService(BATTERY_SERVICE)) {
            if (auto *b = svc->getCharacteristic(BATTERY_CHAR)) {
                if (b->canRead()) {
                    std::string v = b->readValue();
                    if (!v.empty()) battery = (uint8_t)v[0];
                }
                if (b->canNotify()) b->subscribe(true, onBattery);
            }
        }
    }

    carName = car.name;
    ctl = {};
    lights = turbo = donut = false;
    mode = 1;
    packets = 0;
    lastSend = 0;
    notice = "";
    screen = Screen::Driving;
    return true;
}

static size_t buildPacket(uint8_t *out)
{
    if (gen == GEN_2024) {
        uint8_t plain[16] = {0, 'C', 'T', 'L'};
        plain[4] = ctl.fwd;
        plain[5] = ctl.back;
        plain[6] = ctl.left;
        plain[7] = ctl.right;
        plain[9] = SPEEDS_2024[speedIdx];

        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, AES_KEY_2024, 128);
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, plain, out);
        mbedtls_aes_free(&aes);
        return 16;
    }
    out[0] = mode;
    out[1] = ctl.fwd;
    out[2] = ctl.back;
    out[3] = ctl.left;
    out[4] = ctl.right;
    out[5] = lights;
    out[6] = turbo;
    out[7] = donut;
    return 8;
}

static bool sendRaw(const uint8_t *data, size_t len)
{
    if (!writeChr) return false;
    bool withResponse = !writeChr->canWriteNoResponse();
    return writeChr->writeValue(data, len, withResponse);
}

static void sendControls()
{
    uint8_t pkt[16];
    size_t len = buildPacket(pkt);
    if (sendRaw(pkt, len)) packets++;
    lastSend = millis();
}

static void disconnectCar()
{
    if (client && client->isConnected()) {
        ctl = {};
        turbo = donut = false;
        for (int i = 0; i < 3; i++) {
            if (gen == GEN_2024) sendRaw(IDLE_2024, 16);
            else sendControls();
            delay(SEND_MS);
        }
        client->disconnect();
    }
    writeChr = nullptr;
    screen = Screen::List;
    startScan();
}

// --- Dibujo ------------------------------------------------------------------------
static void drawHud(const char *center, const char *right)
{
    canvas.fillRect(0, 0, SCREEN_W, HUD_H, C_HUD);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextColor(C_YELLOW, C_HUD);
    canvas.setTextDatum(middle_left);
    canvas.drawString("SHELL RC", 4, HUD_H / 2);
    canvas.setTextColor(C_TEXT, C_HUD);
    canvas.setTextDatum(middle_center);
    canvas.drawString(center, SCREEN_W / 2, HUD_H / 2);
    canvas.setTextDatum(middle_right);
    canvas.drawString(right, SCREEN_W - 4, HUD_H / 2);
}

static void drawFooter(const char *text)
{
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(text, SCREEN_W / 2, SCREEN_H - 2);
}

static void drawMessage(const char *title, const char *line)
{
    canvas.fillScreen(C_BG);
    drawHud(GENS[gen].label, "");
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_ACCENT, C_BG);
    canvas.drawString(title, SCREEN_W / 2, 60);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(line, SCREEN_W / 2, 82);
    canvas.pushSprite(0, 0);
}

static void drawPickGen()
{
    drawHud("Generacion", "v" APP_VERSION);
    static const char *DESC[2] = {"QCAR-...  cifrado AES", "SL-...  luces, turbo, donut"};
    for (int i = 0; i < 2; i++) {
        int y = 28 + i * 44;
        bool sel = gen == i;
        canvas.fillRoundRect(20, y, 200, 36, 6, sel ? C_HUD : C_BG);
        canvas.drawRoundRect(20, y, 200, 36, 6, sel ? C_YELLOW : C_DIM);
        canvas.setTextDatum(middle_left);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(sel ? C_YELLOW : C_TEXT, sel ? C_HUD : C_BG);
        canvas.drawString(String(i + 1) + "  Shell " + GENS[i].label, 32, y + 12);
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(C_DIM, sel ? C_HUD : C_BG);
        canvas.drawString(DESC[i], 48, y + 27);
    }
    drawFooter("; . elegir   enter buscar autos");
}

static void drawList()
{
    String hud = String("Autos ") + GENS[gen].label;
    drawHud(hud.c_str(), scanning ? "buscando..." : "");

    canvas.setFont(&fonts::Font0);
    if (cars.empty()) {
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(C_TEXT, C_BG);
        canvas.drawString(scanning ? "Buscando autos..." : "No encontre autos", SCREEN_W / 2, 55);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("Prende el auto y acercalo", SCREEN_W / 2, 72);
    }
    for (int i = 0; i < (int)cars.size(); i++) {
        int y = 20 + i * 12;
        bool sel = i == selected;
        if (sel) canvas.fillRect(4, y - 1, SCREEN_W - 8, 11, C_HUD);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(sel ? C_YELLOW : C_TEXT, sel ? C_HUD : C_BG);
        canvas.drawString((sel ? "> " : "  ") + String(cars[i].name.c_str()), 8, y + 1);
        // Señal en barras: de -90 dBm (nada) a -50 dBm (llena).
        int bars = constrain((cars[i].rssi + 90) / 10, 0, 4);
        for (int b = 0; b < 4; b++) {
            int h = 2 + b * 2;
            canvas.fillRect(SCREEN_W - 40 + b * 4, y + 9 - h, 3, h, b < bars ? C_ON : C_OFF);
        }
        canvas.setTextDatum(top_right);
        canvas.setTextColor(C_DIM, sel ? C_HUD : C_BG);
        canvas.drawString(String(cars[i].rssi), SCREEN_W - 44, y + 1);
    }
    if (notice.length()) {
        canvas.setTextDatum(bottom_center);
        canvas.setTextColor(C_RED, C_BG);
        canvas.drawString(notice, SCREEN_W / 2, SCREEN_H - 13);
    }
    drawFooter("enter conectar  r buscar  del volver");
}

static void drawArrow(int cx, int cy, int dir, bool on)  // 0 arriba, 1 abajo, 2 izq, 3 der
{
    const int s = 13;
    int x0, y0, x1, y1, x2, y2;
    switch (dir) {
        case 0: x0 = cx; y0 = cy - s; x1 = cx - s; y1 = cy + s / 2; x2 = cx + s; y2 = cy + s / 2; break;
        case 1: x0 = cx; y0 = cy + s; x1 = cx - s; y1 = cy - s / 2; x2 = cx + s; y2 = cy - s / 2; break;
        case 2: x0 = cx - s; y0 = cy; x1 = cx + s / 2; y1 = cy - s; x2 = cx + s / 2; y2 = cy + s; break;
        default: x0 = cx + s; y0 = cy; x1 = cx - s / 2; y1 = cy - s; x2 = cx - s / 2; y2 = cy + s; break;
    }
    if (on) canvas.fillTriangle(x0, y0, x1, y1, x2, y2, C_YELLOW);
    else canvas.drawTriangle(x0, y0, x1, y1, x2, y2, C_DIM);
}

static void drawChip(int x, int y, const char *label, bool on)
{
    canvas.fillRoundRect(x, y, 52, 16, 4, on ? C_ON : C_HUD);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(on ? C_BG : C_DIM, on ? C_ON : C_HUD);
    canvas.drawString(label, x + 26, y + 8);
}

static void drawDriving()
{
    int bat = battery;
    String right = gen == GEN_2025 && bat >= 0 ? String(bat) + "%" : String(GENS[gen].label);
    drawHud(carName.c_str(), right.c_str());

    // Cruz de direcciones
    const int cx = 62, cy = 70;
    canvas.drawCircle(cx, cy, 44, C_HUD);
    drawArrow(cx, cy - 26, 0, ctl.fwd);
    drawArrow(cx, cy + 26, 1, ctl.back);
    drawArrow(cx - 26, cy, 2, ctl.left);
    drawArrow(cx + 26, cy, 3, ctl.right);

    canvas.setFont(&fonts::Font0);
    const int px = 128;
    if (gen == GEN_2024) {
        canvas.setTextDatum(top_left);
        canvas.setTextColor(C_ACCENT, C_BG);
        canvas.drawString("VELOCIDAD", px, 30);
        for (int i = 0; i < 4; i++) {
            int h = 10 + i * 8;
            uint16_t c = i <= speedIdx ? (i == 3 ? C_RED : C_YELLOW) : C_OFF;
            canvas.fillRect(px + i * 22, 84 - h, 16, h, c);
            canvas.setTextColor(C_DIM, C_BG);
            canvas.drawString(String(i + 1), px + i * 22 + 5, 88);
        }
    } else {
        drawChip(px, 26, "l LUCES", lights);
        drawChip(px + 56, 26, "t TURBO", turbo);
        drawChip(px, 48, "o DONUT", donut);
        drawChip(px + 56, 48, mode == 1 ? "m MODO 1" : "m MODO 2", mode == 2);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(C_ACCENT, C_BG);
        canvas.drawString("BATERIA", px, 74);
        int pct = bat < 0 ? 0 : min(bat, 100);
        canvas.drawRect(px, 85, 100, 10, C_DIM);
        canvas.fillRect(px + 1, 86, pct * 98 / 100, 8, pct < 20 ? C_RED : C_ON);
        if (bat < 0) {
            canvas.setTextColor(C_DIM, C_BG);
            canvas.drawString("sin dato", px + 52, 74);
        }
    }
    drawFooter(gen == GEN_2024 ? ";.,/ manejar  1-4 velocidad  del salir" : ";.,/ manejar   del desconectar");
}

static void render()
{
    canvas.fillScreen(C_BG);
    switch (screen) {
        case Screen::PickGen: drawPickGen(); break;
        case Screen::List: drawList(); break;
        case Screen::Driving: drawDriving(); break;
    }
    canvas.pushSprite(0, 0);
}

// --- Entrada ---------------------------------------------------------------------
static bool hasChar(const Keyboard_Class::KeysState &ks, const char *chars)
{
    for (char c : ks.word) {
        if (strchr(chars, c)) return true;
    }
    return false;
}

static void handlePickGen(const Keyboard_Class::KeysState &ks)
{
    if (hasChar(ks, ";w1")) gen = GEN_2024;
    if (hasChar(ks, ".s2")) gen = GEN_2025;
    if (hasChar(ks, "12") || ks.enter) {
        prefs.putUChar("gen", gen);
        notice = "";
        screen = Screen::List;
        startScan();
    }
}

static void handleList(const Keyboard_Class::KeysState &ks)
{
    if (ks.del || hasChar(ks, "`")) {
        stopScan();
        screen = Screen::PickGen;
        return;
    }
    if (hasChar(ks, "rR")) {
        notice = "";
        startScan();
        return;
    }
    if (cars.empty()) return;
    if (hasChar(ks, ";w") && selected > 0) selected--;
    if (hasChar(ks, ".s") && selected < (int)cars.size() - 1) selected++;
    if (ks.enter && !connectTo(cars[selected])) startScan();
}

// Teclas de una sola vez en la pantalla de manejo (las de dirección se leen mantenidas).
static void handleDrivingPress(const Keyboard_Class::KeysState &ks)
{
    if (ks.del || hasChar(ks, "`")) {
        disconnectCar();
        return;
    }
    if (gen == GEN_2024) {
        for (char c : ks.word) {
            if (c >= '1' && c <= '4') speedIdx = c - '1';
        }
    } else {
        if (hasChar(ks, "lL")) lights = !lights;
        if (hasChar(ks, "tT")) turbo = !turbo;
        if (hasChar(ks, "oO")) donut = !donut;
        if (hasChar(ks, "mM")) mode = mode == 1 ? 2 : 1;
    }
    sendControls();  // que el cambio llegue ya, sin esperar al próximo envío
}

static void readDirections()
{
    Controls c = {};
    if (M5Cardputer.Keyboard.isPressed()) {
        auto ks = M5Cardputer.Keyboard.keysState();
        c.fwd = hasChar(ks, ";wW");
        c.back = hasChar(ks, ".sS");
        c.left = hasChar(ks, ",aA");
        c.right = hasChar(ks, "/dD");
        // Opuestas a la vez se anulan.
        if (c.fwd && c.back) c.fwd = c.back = false;
        if (c.left && c.right) c.left = c.right = false;
    }
    if (memcmp(&c, &ctl, sizeof(c)) != 0) {
        ctl = c;
        dirty = true;
        sendControls();
    }
}

static void handleKeys()
{
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto ks = M5Cardputer.Keyboard.keysState();
        dirty = true;
        switch (screen) {
            case Screen::PickGen: handlePickGen(ks); break;
            case Screen::List: handleList(ks); break;
            case Screen::Driving: handleDrivingPress(ks); break;
        }
    }
    if (screen == Screen::Driving) readDirections();
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    canvas.setColorDepth(16);
    canvas.createSprite(SCREEN_W, SCREEN_H);

    C_BG = canvas.color565(12, 12, 16);
    C_HUD = canvas.color565(34, 30, 30);
    C_TEXT = canvas.color565(225, 225, 230);
    C_DIM = canvas.color565(120, 120, 130);
    C_ACCENT = canvas.color565(255, 214, 0);  // amarillo Shell
    C_YELLOW = canvas.color565(255, 214, 0);
    C_RED = canvas.color565(221, 29, 33);     // rojo Shell
    C_ON = canvas.color565(80, 210, 110);
    C_OFF = canvas.color565(50, 50, 58);

    prefs.begin("shell-rc", false);
    gen = (Gen)min<uint8_t>(prefs.getUChar("gen", GEN_2025), 1);

    NimBLEDevice::init("Cardputer RC");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    render();
}

void loop()
{
    handleKeys();
    uint32_t now = millis();

    if (screen == Screen::List) {
        if (now - lastScanPoll >= 400) {
            lastScanPoll = now;
            bool was = scanning;
            scanning = NimBLEDevice::getScan()->isScanning();
            if (foundChanged) {
                collectCars();
                dirty = true;
            }
            if (was != scanning) dirty = true;
        }
    }

    if (screen == Screen::Driving) {
        if (!client || !client->isConnected()) {
            writeChr = nullptr;
            notice = "Se perdio la conexion";
            screen = Screen::List;
            startScan();
        } else if (now - lastSend >= SEND_MS) {
            sendControls();
        }
        if (batteryChanged) {
            batteryChanged = false;
            dirty = true;
        }
    }

    if (dirty) {
        render();
        dirty = false;
    }
    delay(2);
}
