// Sumo RC — control remoto del Parrot Jumping Sumo desde la Cardputer.
//
// El Sumo crea su propia red WiFi (JumpingSumo_xxxxxx, sin clave). La app se une a esa red, hace el
// handshake por TCP (192.168.2.1:44444) y habla con el robot por UDP: comandos de manejo a 40 Hz y
// video MJPEG de la cámara, que el OS decodifica a 160x120.
//
// Teclas (las flechas impresas en el teclado):
//   ;  .  ,  /    adelante / atrás / izquierda / derecha (mientras las mantenés apretadas)
//   -  =          velocidad menos / más
//   j  l          salto alto / salto largo
//   1..9          animaciones (1 giro, 2 golpecito, 3 temblor, 4 metrónomo, 5 ondulación,
//                 6 giro+salto, 7 giro a postura, 8 espiral, 9 slalom)
//   u  k  y       postura: saltador / pateador / parado
//   space         frena y corta la animación        v  video on/off
// Soltar las teclas de manejo frena al robot. Para salir: Fn + `.

#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <vector>
#include <string>

#include "sumo_core.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HUD_H = 15;
static constexpr int VID_W = 160;
static constexpr int VID_H = 120;
static constexpr uint32_t PCMD_MS = 25;
static constexpr uint32_t LINK_TIMEOUT_MS = 3000;
static const uint32_t SUMO_IP = 0x0102A8C0;  // 192.168.2.1 en orden de red (lwip)
static constexpr int DISCOVERY_PORT = 44444;
static constexpr int D2C_PORT = 43210;

static M5Canvas canvas(&M5Cardputer.Display);
static uint16_t C_BG, C_HUD, C_TEXT, C_DIM, C_ACCENT, C_OK, C_WARN, C_ERR, C_VID;

enum class Screen { Scan, Connecting, Drive };
static Screen screen = Screen::Scan;
static bool dirty = true;

// JPEG desde memoria para el decodificador del OS (cpos_api->media.jpeg_draw)
struct MemReader { const uint8_t *d; size_t n, pos; };
static size_t memRead(void *ctx, uint8_t *buf, size_t n)
{
    auto *m = static_cast<MemReader *>(ctx);
    size_t left = m->n - m->pos;
    if (n > left) n = left;
    if (buf) memcpy(buf, m->d + m->pos, n);  // buf nulo = saltear n bytes
    m->pos += n;
    return n;
}

// --- red --------------------------------------------------------------------------------
struct Net : sumo::Listener {
    int udp = -1;
    cpos_sockaddr_in drone{};
    uint32_t lastRx = 0;
    bool videoOn = true;
    int battery = -1, posture = 0, alert = 0, jumpLoad = 0;
    uint32_t frames = 0, decodeFail = 0, lastDecode = 0;
    bool haveVideo = false;
    bool demo = false;

    void send(const uint8_t *d, size_t n) override
    {
        if (demo || udp < 0) return;
        cpos_api->net.sendto(udp, d, n, 0, &drone, sizeof(drone));
    }
    void on_jpeg(const uint8_t *d, size_t n) override
    {
        frames++;
        // la decodificación tarda: si venimos apurados se saltea el cuadro para no demorar el manejo
        if (millis() - lastDecode < 70) return;
        lastDecode = millis();
        MemReader r{d, n, 0};
        if (cpos_api->media.jpeg_draw(canvas.target(), 0, HUD_H, VID_W, VID_H, memRead, &r) == 0) {  // 640x480 -> 1/4
            haveVideo = true;
            dirty = true;
        } else {
            decodeFail++;
        }
    }
    void on_battery(int p) override { battery = p; dirty = true; }
    void on_posture(int p) override { posture = p; dirty = true; }
    void on_alert(int a) override { alert = a; dirty = true; }
    void on_jump_load(int s) override { jumpLoad = s; }
};

static Net net;
static sumo::Link link_(&net);
static std::string sumoSsid;
static std::string status;

static std::vector<std::string> sumos;
static std::vector<int> sumoRssi;
static int selected = 0;
static bool scanning = false;

static void closeSockets()
{
    if (net.udp >= 0) cpos_api->net.close(net.udp);
    net.udp = -1;
}

static void startScan()
{
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    scanning = true;
    dirty = true;
}

static void collectNets()
{
    int n = WiFi.scanComplete();
    if (n < 0) return;
    sumos.clear();
    sumoRssi.clear();
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        std::string name = s.c_str();
        if (name.find("Sumo") == std::string::npos && name.find("sumo") == std::string::npos) continue;
        sumos.push_back(name);
        sumoRssi.push_back(WiFi.RSSI(i));
    }
    WiFi.scanDelete();
    scanning = false;
    if (selected >= (int)sumos.size()) selected = 0;
    dirty = true;
}

// Conecta al WiFi del robot y hace el handshake. Devuelve false con `status` explicando por qué.
static bool connectToSumo(const std::string &ssid)
{
    status = "Uniéndose a " + ssid;
    canvas.fillScreen(C_BG);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    canvas.drawString("Conectando...", SCREEN_W / 2, 50);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(ssid.c_str(), SCREEN_W / 2, 75);
    canvas.pushSprite(0, 0);

    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), "");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        M5Cardputer.update();
        if (millis() - t0 > 15000) { status = "No pude unirme al WiFi del robot"; return false; }
        delay(100);
    }

    // handshake por TCP
    int tcp = cpos_api->net.socket(2, 1, 0);
    if (tcp < 0) { status = "Sin sockets disponibles"; return false; }
    cpos_sockaddr_in a = {sizeof(a), 2, (uint16_t)(((DISCOVERY_PORT & 0xFF) << 8) | (DISCOVERY_PORT >> 8)), SUMO_IP, {0}};
    if (cpos_api->net.sock_connect(tcp, &a, sizeof(a)) != 0) {
        cpos_api->net.close(tcp);
        status = "El robot no responde (puerto 44444)";
        return false;
    }
    char hello[128];
    snprintf(hello, sizeof(hello), "{\"controller_type\":\"cardputer\",\"controller_name\":\"cardputeros\",\"d2c_port\":\"%d\"}", D2C_PORT);
    cpos_api->net.sock_send(tcp, hello, strlen(hello) + 1, 0);
    cpos_api->net.fcntl(tcp, 4, 1);
    char reply[512] = {0};
    int got = -1;
    for (t0 = millis(); millis() - t0 < 3000 && got <= 0;) {
        got = cpos_api->net.sock_recv(tcp, reply, sizeof(reply) - 1, 0);
        if (got <= 0) delay(20);
    }
    cpos_api->net.close(tcp);
    if (got <= 0) { status = "Sin respuesta del robot"; return false; }
    int c2d = sumo::Link::parse_c2d_port(reply, 54321);

    closeSockets();
    net.udp = cpos_api->net.socket(2, 2, 0);
    if (net.udp < 0) { status = "Sin sockets UDP"; return false; }
    int one = 1;
    cpos_api->net.setsockopt(net.udp, 0xfff, 0x0004, &one, sizeof(one));
    cpos_sockaddr_in me = {sizeof(me), 2, (uint16_t)(((D2C_PORT & 0xFF) << 8) | (D2C_PORT >> 8)), 0, {0}};
    if (cpos_api->net.bind(net.udp, &me, sizeof(me)) != 0) { closeSockets(); status = "No pude abrir el puerto de video"; return false; }
    cpos_api->net.fcntl(net.udp, 4, 1);
    net.drone = {sizeof(net.drone), 2, (uint16_t)(((c2d & 0xFF) << 8) | (c2d >> 8)), SUMO_IP, {0}};
    net.lastRx = millis();
    link_.send_all_states();
    link_.send_video_enable(net.videoOn);
    return true;
}

// --- dibujo -----------------------------------------------------------------------------
static const char *postureName(int p)
{
    switch (p) {
        case 0: return "parado";
        case 1: return "saltador";
        case 2: return "pateador";
        case 3: return "trabado";
    }
    return "?";
}

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
    drawHud("SUMO RC", "v" APP_VERSION);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::lgfxJapanGothicP_12);
    if (sumos.empty()) {
        canvas.setTextColor(C_TEXT, C_BG);
        canvas.drawString(scanning ? "Buscando el WiFi del Sumo..." : "No encontré ningún Sumo", 10, 30);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("Prendelo y acercalo (red JumpingSumo_...)", 10, 50);
    } else {
        for (int i = 0; i < (int)sumos.size() && i < 6; i++) {
            int y = 24 + i * 18;
            if (i == selected) canvas.fillRoundRect(4, y - 2, SCREEN_W - 8, 17, 4, C_HUD);
            canvas.setTextColor(i == selected ? C_ACCENT : C_TEXT, i == selected ? C_HUD : C_BG);
            canvas.drawString(sumos[i].c_str(), 12, y);
            char r[16];
            snprintf(r, sizeof(r), "%d dBm", sumoRssi[i]);
            canvas.setTextDatum(top_right);
            canvas.setTextColor(C_DIM, i == selected ? C_HUD : C_BG);
            canvas.drawString(r, SCREEN_W - 10, y);
            canvas.setTextDatum(top_left);
        }
    }
    if (!status.empty()) {
        canvas.setTextColor(C_ERR, C_BG);
        canvas.drawString(status.c_str(), 10, 104);
    }
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("enter conectar  r buscar  d demo", 10, 120);
    canvas.pushSprite(0, 0);
}

static int speedPct = 50;
static bool moving = false;

static bool clearDrive = true;  // la primera vez se limpia todo: no quedan restos de la pantalla anterior

static void renderDrive()
{
    if (clearDrive) { canvas.fillScreen(C_BG); clearDrive = false; }
    drawHud(net.demo ? "SUMO RC  demo" : sumoSsid.c_str(), net.alert == 1 ? "BATERIA CRITICA" : (net.alert == 2 ? "bateria baja" : "Fn+` salir"));
    // video (se dibuja solo cuando llega un cuadro); marco y placeholder
    if (!net.haveVideo) {
        canvas.fillRect(0, HUD_H, VID_W, VID_H, C_VID);
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(C_DIM, C_VID);
        canvas.setTextDatum(middle_center);
        canvas.drawString(net.videoOn ? "esperando video..." : "video apagado", VID_W / 2, HUD_H + VID_H / 2);
    }
    // panel derecho
    const int px = VID_W + 4, pw = SCREEN_W - px;
    canvas.fillRect(px, HUD_H, pw, VID_H, C_BG);
    canvas.setFont(&fonts::lgfxJapanGothicP_12);
    canvas.setTextDatum(top_left);
    char b[32];
    if (net.battery >= 0) snprintf(b, sizeof(b), "Bat %d%%", net.battery); else snprintf(b, sizeof(b), "Bat --");
    uint16_t bc = net.battery < 0 ? C_DIM : (net.battery < 20 ? C_ERR : (net.battery < 40 ? C_WARN : C_OK));
    canvas.setTextColor(bc, C_BG);
    canvas.drawString(b, px, HUD_H + 3);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(postureName(net.posture), px, HUD_H + 18);
    // velocidad
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("Vel", px, HUD_H + 36);
    canvas.drawRoundRect(px + 24, HUD_H + 38, pw - 28, 8, 3, C_DIM);
    canvas.fillRoundRect(px + 25, HUD_H + 39, (pw - 30) * speedPct / 100, 6, 2, C_ACCENT);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(moving ? C_OK : C_DIM, C_BG);
    canvas.drawString(moving ? "en marcha" : "frenado", px, HUD_H + 52);
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString(";.,/ mover", px, HUD_H + 70);
    canvas.drawString("j l  saltos", px, HUD_H + 80);
    canvas.drawString("1-9 anim", px, HUD_H + 90);
    canvas.drawString("ukY postura", px, HUD_H + 100);
    canvas.drawString("-= vel v vid", px, HUD_H + 110);
    if (!net.demo && millis() - net.lastRx > LINK_TIMEOUT_MS) {
        canvas.fillRoundRect(10, 50, 140, 30, 4, C_ERR);
        canvas.setTextColor(C_TEXT, C_ERR);
        canvas.setTextDatum(middle_center);
        canvas.drawString("SIN SEÑAL DEL ROBOT", 80, 65);
    }
    canvas.pushSprite(0, 0);
}

// --- manejo -----------------------------------------------------------------------------
static uint32_t lastPcmd = 0;

static void handleKeys()
{
    M5Cardputer.update();
    auto ks = M5Cardputer.Keyboard.keysState();

    bool fwd = false, back = false, left = false, right = false;
    for (char c : ks.word) {
        switch (c) {
            case ';': fwd = true; break;
            case '.': back = true; break;
            case ',': left = true; break;
            case '/': right = true; break;
        }
    }
    int speed = (fwd ? speedPct : 0) - (back ? speedPct : 0);
    int turn = (right ? 1 : 0) - (left ? 1 : 0);
    int turnVal = turn * (speedPct < 30 ? 30 : speedPct);
    bool active = speed != 0 || turn != 0;
    if (active != moving) { moving = active; dirty = true; }

    if (millis() - lastPcmd >= PCMD_MS) {
        lastPcmd = millis();
        link_.send_pcmd(active, speed, turnVal);  // sin teclas: flag 0 = el robot frena
    }

    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    for (char c : ks.word) {
        switch (c) {
            case '-': speedPct = speedPct > 10 ? speedPct - 10 : 10; dirty = true; break;
            case '=': case '+': speedPct = speedPct < 100 ? speedPct + 10 : 100; dirty = true; break;
            case 'j': link_.send_jump(sumo::JUMP_HIGH); break;
            case 'l': link_.send_jump(sumo::JUMP_LONG); break;
            case '1': link_.send_animation(sumo::ANIM_SPIN); break;
            case '2': link_.send_animation(sumo::ANIM_TAP); break;
            case '3': link_.send_animation(sumo::ANIM_SLOWSHAKE); break;
            case '4': link_.send_animation(sumo::ANIM_METRONOME); break;
            case '5': link_.send_animation(sumo::ANIM_ONDULATION); break;
            case '6': link_.send_animation(sumo::ANIM_SPINJUMP); break;
            case '7': link_.send_animation(sumo::ANIM_SPINTOPOSTURE); break;
            case '8': link_.send_animation(sumo::ANIM_SPIRAL); break;
            case '9': link_.send_animation(sumo::ANIM_SLALOM); break;
            case 'u': link_.send_posture(sumo::POSTURE_JUMPER); break;
            case 'k': link_.send_posture(sumo::POSTURE_KICKER); break;
            case 'y': link_.send_posture(sumo::POSTURE_STANDING); break;
            case ' ': link_.send_animation(sumo::ANIM_STOP); break;
            case 'v':
                net.videoOn = !net.videoOn;
                net.haveVideo = false;
                link_.send_video_enable(net.videoOn);
                dirty = true;
                break;
        }
    }
    if (ks.space) link_.send_animation(sumo::ANIM_STOP);
}

static void pollNet()
{
    if (net.demo || net.udp < 0) return;
    static uint8_t buf[1536];
    for (int i = 0; i < 12; i++) {
        int n = cpos_api->net.recvfrom(net.udp, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) break;
        net.lastRx = millis();
        link_.on_datagram(buf, n);
    }
}

// --- Arduino ---------------------------------------------------------------------------
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
            if (scanning && WiFi.scanComplete() >= 0) collectNets();
            if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
                auto ks = M5Cardputer.Keyboard.keysState();
                if (ks.enter && !sumos.empty()) {
                    sumoSsid = sumos[selected];
                    status.clear();
                    screen = Screen::Connecting;
                    break;
                }
                for (char c : ks.word) {
                    if (c == ';' && selected > 0) { selected--; dirty = true; }
                    else if (c == '.' && selected + 1 < (int)sumos.size()) { selected++; dirty = true; }
                    else if (c == 'r' || c == 'R') { status.clear(); startScan(); }
                    else if (c == 'd' || c == 'D') {
                        net.demo = true;
                        net.battery = 87;
                        sumoSsid = "demo";
                        clearDrive = true;
                        screen = Screen::Drive;
                        dirty = true;
                    }
                }
            }
            if (dirty) { renderScan(); dirty = false; }
            delay(10);
            break;
        }
        case Screen::Connecting:
            if (connectToSumo(sumoSsid)) {
                net.haveVideo = false;
                clearDrive = true;
                screen = Screen::Drive;
            } else {
                screen = Screen::Scan;
                startScan();
            }
            dirty = true;
            break;
        case Screen::Drive: {
            handleKeys();
            pollNet();
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
