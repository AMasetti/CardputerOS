// Meshtastic por Bluetooth para M5Stack Cardputer-Adv con el Cap LoRa-1262.
//
// Versión sin interfaz: el Cardputer es el nodo (radio LoRa, GPS, retransmisión) y el teléfono es la
// pantalla. La app oficial de Meshtastic (iPhone o Android) se conecta por Bluetooth como a cualquier
// nodo: chat, lista de nodos, mapa, y la configuración del nodo (nombre, región, modo, canal) se cambia
// desde el teléfono. La pantalla solo muestra los logos y dos puntos de estado:
//   abajo a la izquierda  Bluetooth   gris: esperando · azul: teléfono conectado
//   abajo a la derecha    LoRa        verde: radio lista · rojo: no responde el Cap LoRa-1262
//
// Comparte con la app Meshtastic (con pantalla) la identidad, la configuración y la lista de nodos
// (en la SD, /meshtastic), así que es el mismo nodo con cualquiera de las dos.
//
//   Fn + `   salir

#include <M5Cardputer.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include "mesh_core.h"
#include "phone_api.h"
#include "settings.h"
#include "gnss.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

using namespace mesh;

static Preferences prefs;
static Settings S;
static bool okMqtt = true;
static uint32_t myNode;

static Router *g_router;
#define R (*g_router)
static phone::PhoneApi *g_api;
#define API (*g_api)

static bool radioUp = false;

// ---- ajustes -----------------------------------------------------------------------------------------------
static void loadSettings() {
    prefs.begin("meshtastic-ble", false);
    myNode = prefs.getUInt("node", 0);
    if (myNode < 0x10 || myNode == BROADCAST) {
        do myNode = esp_random(); while (myNode < 0x10 || myNode == BROADCAST);
        prefs.putUInt("node", myNode);
    }
    settings_default(S, myNode);
    Settings tmp;
    if (prefs.getBytes("cfg", &tmp, sizeof(tmp)) == sizeof(tmp)) S = tmp;
    settings_sanitize(S);
    okMqtt = prefs.getBool("mqtt", true);
    uint32_t node;
    if (settings_load_shared(node, tmp, okMqtt)) { // la misma identidad que la app con pantalla
        myNode = node;
        S = tmp;
    }
}

static void saveSettings() {
    prefs.putBytes("cfg", &S, sizeof(S));
    prefs.putUInt("node", myNode);
    prefs.putBool("mqtt", okMqtt);
    settings_save_shared(myNode, S, okMqtt);
}

// ---- radio ------------------------------------------------------------------------------------------------------
static const Region &region() { return REGIONS[S.region]; }
static const Preset &preset() { return PRESETS[S.preset]; }
static const char *chanName() { return S.channel[0] ? S.channel : preset().name; }

static cpos_lora_cfg_t radioCfg() {
    cpos_lora_cfg_t c = {};
    c.freq_hz = frequency_hz(region(), preset(), chanName(), S.slot);
    c.bw_hz = preset().bw_hz;
    c.sf = preset().sf;
    c.cr = preset().cr;
    c.sync_word = SYNC_WORD;
    c.power_dbm = S.power > region().max_power ? region().max_power : S.power;
    c.preamble = PREAMBLE;
    c.crc = 1;
    return c;
}

static uint32_t p_now() { return millis(); }
static uint32_t p_rnd() { return esp_random(); }
static int p_aes(const uint8_t *key, int bits, const uint8_t in[16], uint8_t out[16]) {
    return cpos_api->crypto.aes_ecb(key, bits, 1, in, out);
}
static int p_send(const uint8_t *p, size_t n) { return radioUp ? cpos_api->lora.send(p, n) : 0; }
static int p_busy() { return radioUp && (cpos_api->lora.busy() || cpos_api->lora.pending() > 1); }

static void applyRouter() {
    R.me = myNode;
    R.hop_limit = S.hops;
    R.rebroadcast = S.relay;
    R.ok_to_mqtt = okMqtt;
    R.preset = preset();
    snprintf(R.owner.id, sizeof(R.owner.id), "!%08x", (unsigned)myNode);
    strncpy(R.owner.long_name, S.long_name, sizeof(R.owner.long_name) - 1);
    strncpy(R.owner.short_name, S.short_name, sizeof(R.owner.short_name) - 1);
    R.owner.hw_model = HW_M5STACK_CARDPUTER_ADV;
    R.set_channel(S.channel, S.psk, S.psk_len);
    // lo que el teléfono ve de este nodo
    API.S.region_code = region().code;
    API.S.preset_code = preset().code;
    API.S.hop_limit = S.hops;
    API.S.tx_power = S.power;
    API.S.slot = S.slot;
    API.S.ok_to_mqtt = okMqtt;
    API.S.psk = S.psk;
    API.S.psk_len = S.psk_len;
    API.S.channel_name = S.channel;
}

static void startRadio() {
    cpos_lora_cfg_t c = radioCfg();
    if (radioUp) cpos_api->lora.configure(&c);
    else radioUp = cpos_api->lora.begin(&c) == 0;
}

// ---- nodos en la SD (los mismos que la app con pantalla) ---------------------------------------------------------
static const char *NODES_FILE = "/sdcard/meshtastic/nodes.bin";
static const uint32_t FILE_MAGIC = 0x4D534831; // "MSH1"
static bool nodesDirty = false;
static uint32_t lastSave = 0;

static void loadNodes() {
    FILE *f = fopen(NODES_FILE, "rb");
    if (!f) return;
    uint32_t h[3] = {0};
    if (fread(h, sizeof(h), 1, f) == 1 && h[0] == FILE_MAGIC && h[1] == sizeof(Node)) {
        uint32_t n = h[2] > (uint32_t)Router::MAX_NODES ? Router::MAX_NODES : h[2];
        R.num_nodes = (int)fread(R.nodes, sizeof(Node), n, f);
        for (int i = 0; i < R.num_nodes; i++) R.nodes[i].last_heard = 0;
    }
    fclose(f);
}

static void saveNodes(bool force = false) {
    if (!nodesDirty || (!force && millis() - lastSave < 60000)) return;
    mkdir("/sdcard/meshtastic", 0777);
    FILE *f = fopen("/sdcard/meshtastic/nodes.bin.tmp", "wb");
    if (!f) return;
    uint32_t h[3] = {FILE_MAGIC, sizeof(Node), (uint32_t)R.num_nodes};
    bool ok = fwrite(h, sizeof(h), 1, f) == 1 && (R.num_nodes == 0 || fwrite(R.nodes, sizeof(Node), R.num_nodes, f) == (size_t)R.num_nodes);
    fclose(f);
    if (ok) { remove(NODES_FILE); rename("/sdcard/meshtastic/nodes.bin.tmp", NODES_FILE); }
    nodesDirty = false;
    lastSave = millis();
}

// ---- eventos ----------------------------------------------------------------------------------------------------
struct App : Listener {
    void on_text(const Header &, const Data &, const Node *) override {} // el teléfono lo recibe por on_packet
    void on_ack(uint32_t id, bool ok, uint32_t err) override { API.on_ack(id, ok, err); }
    void on_node(const Node &) override { nodesDirty = true; }
    void on_packet(const Header &h, const Data &d, int16_t rssi, float snr) override { API.on_mesh_packet(h, d, rssi, snr); }
} app;

// ---- configuración desde el teléfono ---------------------------------------------------------------------------
static int regionIndex(uint32_t code) {
    for (int i = 0; i < NUM_REGIONS; i++) if (REGIONS[i].code == code) return i;
    return -1;
}
static int presetIndex(uint32_t code) {
    for (int i = 0; i < NUM_PRESETS; i++) if (PRESETS[i].code == code) return i;
    return -1;
}

static void adminOwner(const User &u) {
    if (u.long_name[0]) strncpy(S.long_name, u.long_name, sizeof(S.long_name) - 1);
    if (u.short_name[0]) { strncpy(S.short_name, u.short_name, sizeof(S.short_name) - 1); S.short_name[4] = 0; }
    saveSettings();
    applyRouter();
    R.send_nodeinfo();
}

static void adminLora(uint32_t region, uint32_t preset, uint32_t hops, int32_t power, uint32_t slot, bool ok) {
    int ri = regionIndex(region), pi = presetIndex(preset);
    if (ri >= 0) S.region = (uint8_t)ri;   // regiones fuera de 915 MHz no las cubre el módulo: se ignoran
    if (pi >= 0) S.preset = (uint8_t)pi;
    if (hops >= 1 && hops <= 7) S.hops = (uint8_t)hops;
    if (power >= 2 && power <= 22) S.power = (int8_t)power;
    S.slot = (uint16_t)slot;
    okMqtt = ok;
    saveSettings();
    applyRouter();
    startRadio();
}

static void adminChannel(const char *name, const uint8_t *psk, size_t len) {
    if (len != 0 && len != 1 && len != 16 && len != 32) return;
    strncpy(S.channel, name, sizeof(S.channel) - 1);
    S.channel[sizeof(S.channel) - 1] = 0;
    memcpy(S.psk, psk, len);
    S.psk_len = (uint8_t)len;
    saveSettings();
    applyRouter();
    startRadio();
}

static void adminTime(uint32_t t) { gnss_set_time(t); }

// ---- Bluetooth: el servicio de Meshtastic ------------------------------------------------------------------------
// UUIDs de la app oficial (firmware/src/BluetoothCommon.h), en el orden de bytes del aire
static void uuid(const char *s, uint8_t out[16]) {
    uint8_t be[16];
    int n = 0;
    for (const char *p = s; *p && n < 16; p++) {
        if (*p == '-') continue;
        auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        be[n++] = (uint8_t)(hex(p[0]) << 4 | hex(p[1]));
        p++;
    }
    for (int i = 0; i < 16; i++) out[i] = be[15 - i];
}

enum { CHR_TORADIO, CHR_FROMRADIO, CHR_FROMNUM };
static bool bleUp = false;
static uint32_t fromNum = 0;

static void startBle() {
    uint8_t svc[16];
    cpos_gatt_chr_t chrs[3];
    uuid("6ba1b218-15a8-461f-9fa8-5dcae273eafd", svc);
    uuid("f75c76d2-129e-4dad-a1dd-7866124401e7", chrs[CHR_TORADIO].uuid);
    chrs[CHR_TORADIO].flags = CPOS_GATT_WRITE;
    uuid("2c55e69e-4993-11ed-b878-0242ac120002", chrs[CHR_FROMRADIO].uuid);
    chrs[CHR_FROMRADIO].flags = CPOS_GATT_READ;
    uuid("ed9da18c-a800-4f66-a670-aa7547e34453", chrs[CHR_FROMNUM].uuid);
    chrs[CHR_FROMNUM].flags = CPOS_GATT_NOTIFY;
    char name[24];
    snprintf(name, sizeof(name), "%s_%04x", S.short_name, (unsigned)(myNode & 0xFFFF)); // como el firmware
    bleUp = cpos_api->ble_srv.serve(name, svc, chrs, 3) == 0;
}

static void pollBle() {
    if (!bleUp) return;
    static uint8_t buf[CPOS_GATT_MAX_VALUE];
    int chr = -1, n;
    while ((n = cpos_api->ble_srv.written(&chr, buf, sizeof(buf))) >= 0)
        if (chr == CHR_TORADIO) API.on_to_radio(buf, (size_t)n);
    if (!cpos_api->ble_srv.connected() && API.connected) API.reset(); // el teléfono se fue
    // lo que hay para el teléfono, mientras entre en la cola de lecturas
    size_t len;
    bool any = false;
    while (cpos_api->ble_srv.queued(CHR_FROMRADIO) < 4 && API.next(buf, sizeof(buf), len)) {
        cpos_api->ble_srv.push(CHR_FROMRADIO, buf, len);
        any = true;
    }
    if (any && API.connected) { // avisa por FromNum que hay paquetes nuevos
        fromNum++;
        uint8_t v[4] = {(uint8_t)fromNum, (uint8_t)(fromNum >> 8), (uint8_t)(fromNum >> 16), (uint8_t)(fromNum >> 24)};
        cpos_api->ble_srv.notify(CHR_FROMNUM, v, 4);
    }
}

// ---- pantalla: logos y dos puntos de estado ------------------------------------------------------------------------
static const uint16_t C_BG = lgfx::color565(4, 12, 24);
static const uint16_t C_CYAN = lgfx::color565(64, 228, 255);
static const uint16_t C_MESH = lgfx::color565(103, 234, 148); // verde de Meshtastic
static const uint16_t C_DIM = lgfx::color565(70, 80, 96);

// línea gruesa como dos triángulos
static void thickLine(M5Canvas &c, int oy, float x0, float y0, float x1, float y1, float w, uint16_t col) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0) return;
    float nx = -dy / l * w / 2, ny = dx / l * w / 2;
    c.fillTriangle(x0 + nx, y0 + ny - oy, x1 + nx, y1 + ny - oy, x1 - nx, y1 - ny - oy, col);
    c.fillTriangle(x0 + nx, y0 + ny - oy, x1 - nx, y1 - ny - oy, x0 - nx, y0 - ny - oy, col);
    c.fillCircle(x0, y0 - oy, w / 2, col);
    c.fillCircle(x1, y1 - oy, w / 2, col);
}

static void drawLogos(M5Canvas &c, int oy) {
    c.fillSprite(C_BG);
    // Meshtastic: un trazo y una "V" invertida, en verde
    const int mx = 22, my = 30;
    thickLine(c, oy, mx + 0, my + 50, mx + 26, my + 6, 9, C_MESH);
    thickLine(c, oy, mx + 22, my + 50, mx + 48, my + 6, 9, C_MESH);
    thickLine(c, oy, mx + 48, my + 6, mx + 74, my + 50, 9, C_MESH);
    // CardputerOS: pantalla con prompt y teclas (docs/assets/logo.svg)
    const int cx = 136, cy = 18;
    c.fillRoundRect(cx, cy - oy, 80, 80, 18, C_CYAN);
    c.fillRoundRect(cx + 3, cy + 3 - oy, 74, 74, 16, C_BG);
    c.fillRoundRect(cx + 16, cy + 20 - oy, 48, 34, 7, C_CYAN);
    c.fillRoundRect(cx + 20, cy + 24 - oy, 40, 26, 5, C_BG);
    thickLine(c, oy, cx + 27, cy + 31, cx + 34, cy + 37, 4, C_CYAN);
    thickLine(c, oy, cx + 34, cy + 37, cx + 27, cy + 43, 4, C_CYAN);
    thickLine(c, oy, cx + 39, cy + 44, cx + 52, cy + 44, 4, C_CYAN);
    for (int k = 0; k < 4; k++) c.fillRoundRect(cx + 19 + k * 10, cy + 62 - oy, 7, 3, 1, C_DIM);
    c.setFont(&fonts::lgfxJapanGothicP_12);
    c.setTextColor(C_MESH);
    c.drawCenterString("Meshtastic", mx + 37, 100 - oy);
    c.setTextColor(C_CYAN);
    c.drawCenterString("CardputerOS", cx + 40, 100 - oy);
}

// la pantalla se dibuja una vez en franjas (el canvas se libera: la RAM es para el Bluetooth)
static void drawScreen() {
    M5Canvas c(&M5Cardputer.Display);
    const int band = 27;
    if (!c.createSprite(240, band)) return;
    for (int oy = 0; oy < 135; oy += band) {
        drawLogos(c, oy);
        c.pushSprite(0, oy);
    }
    c.deleteSprite();
}

static void drawStatus() {
    static int last = -1;
    int st = (bleUp && cpos_api->ble_srv.connected() ? 1 : 0) | (radioUp ? 2 : 0) | (bleUp ? 4 : 0);
    if (st == last) return;
    last = st;
    M5Canvas c(&M5Cardputer.Display);
    if (!c.createSprite(12, 12)) return;
    c.fillSprite(C_BG);
    c.fillCircle(5, 5, 4, !bleUp ? lgfx::color565(255, 90, 90) : (st & 1) ? lgfx::color565(60, 140, 255) : C_DIM);
    c.pushSprite(4, 121);
    c.fillSprite(C_BG);
    c.fillCircle(6, 5, 4, radioUp ? C_MESH : lgfx::color565(255, 90, 90));
    c.pushSprite(224, 121);
    c.deleteSprite();
}

// ---- tareas periódicas ---------------------------------------------------------------------------------------------
static Position ownPos;
static DeviceMetrics ownMetrics;
static uint32_t nextNodeInfo = 10000, nextPosition = 60000, nextTelemetry = 120000;

static void periodic() {
    uint32_t t = millis();
    ownMetrics.has = true;
    ownMetrics.battery_level = (uint32_t)M5Cardputer.Power.getBatteryLevel();
    ownMetrics.voltage = M5Cardputer.Power.getBatteryVoltage() / 1000.0f;
    ownMetrics.uptime_seconds = t / 1000;
    if (G.fix) {
        ownPos.has_pos = true;
        ownPos.latitude_i = G.lat;
        ownPos.longitude_i = G.lon;
        ownPos.altitude = G.alt;
        ownPos.time = nowUnix();
        ownPos.location_source = 2;
        ownPos.sats_in_view = G.sats;
        ownPos.precision_bits = 32;
    }
    if ((int32_t)(t - nextNodeInfo) >= 0) {
        if (radioUp) R.send_nodeinfo();
        nextNodeInfo = t + 3u * 3600 * 1000;
    }
    if ((int32_t)(t - nextPosition) >= 0) {
        nextPosition = t + 60000;
        if (radioUp && S.share_pos && G.fix) {
            Position p = ownPos;
            if (psk_is_public(S.psk, S.psk_len)) {
                reduce_precision(p.latitude_i, MAX_PUBLIC_PRECISION);
                reduce_precision(p.longitude_i, MAX_PUBLIC_PRECISION);
                p.precision_bits = MAX_PUBLIC_PRECISION;
            }
            R.send_position(p);
            nextPosition = t + 15u * 60 * 1000;
        }
    }
    if ((int32_t)(t - nextTelemetry) >= 0) {
        nextTelemetry = t + 30u * 60 * 1000;
        if (radioUp) R.send_telemetry(nowUnix(), ownMetrics);
    }
}

// ---- arranque y bucle -----------------------------------------------------------------------------------------------
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    drawScreen();
    loadSettings();
    g_router = new Router();
    g_api = new phone::PhoneApi();
    if (!g_router || !g_api) {
        M5Canvas c(&M5Cardputer.Display);
        if (c.createSprite(240, 14)) {
            c.fillSprite(C_BG);
            c.setFont(&fonts::lgfxJapanGothicP_12);
            c.setTextColor(lgfx::color565(255, 90, 90));
            c.drawCenterString("Sin memoria", 120, 1);
            c.pushSprite(0, 120);
        }
        for (;;) delay(1000);
    }
    Platform pf{p_now, p_rnd, p_aes, p_send, p_busy};
    R.begin(pf, &app);
    API.begin(g_router);
    API.S.now_unix = nowUnix;
    API.S.now_ms = p_now;
    API.own_pos = &ownPos;
    API.own_metrics = &ownMetrics;
    API.admin.set_owner = adminOwner;
    API.admin.set_lora = adminLora;
    API.admin.set_channel = adminChannel;
    API.admin.set_time = adminTime;
    applyRouter();
    loadNodes();
    startRadio();
    if (S.gps) G.up = cpos_api->lora.gnss_begin(115200) == 0;
    startBle();
    drawStatus();
}

void loop() {
    M5Cardputer.update();
    if (radioUp) {
        static cpos_lora_pkt_t pkt;
        while (cpos_api->lora.recv(&pkt)) R.on_receive(pkt.data, pkt.len, pkt.rssi, pkt.snr4 / 4.0f);
        R.loop();
    }
    pollBle();
    pollGnss();
    periodic();
    saveNodes();
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 500) { drawStatus(); lastStatus = millis(); }
    delay(5);
}
