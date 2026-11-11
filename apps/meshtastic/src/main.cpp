// Meshtastic para M5Stack Cardputer-Adv con el Cap LoRa-1262 (SX1262 + GNSS ATGM336H).
//
// Un nodo Meshtastic completo en el Cardputer: chat en el canal público y mensajes directos, lista
// de nodos con distancia, posición por GPS y retransmisión (inundación administrada) para que la
// red crezca. Configurado de fábrica para Argentina: región ANZ (915-928 MHz, la banda que ENACOM
// habilita), LongFast, canal público por defecto. Habla con cualquier nodo Meshtastic 2.x.
//
//   Tab            Chat / Nodos / Config
//   Fn + ; .       arriba / abajo (desplazar, elegir)
//   Fn + , /       Chat: conversación anterior / siguiente   Config: cambiar el valor
//   Enter          Chat: enviar   Nodos: mensaje directo   Config: editar / aplicar
//   Espacio        Nodos: detalle del nodo
//   Fn + `         salir
//
// Los ajustes van a la flash (NVS); nodos y mensajes a la SD (/meshtastic) cuando hay tarjeta.

#include <cpos_intent.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include "mesh_core.h"
#include "mqtt_core.h"
#include <WiFiUdp.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

using namespace mesh;

// ---- colores ---------------------------------------------------------------------------------------
static const uint16_t C_BG = lgfx::color565(8, 12, 20);
static const uint16_t C_BAR = lgfx::color565(20, 28, 44);
static const uint16_t C_TEXT = lgfx::color565(225, 232, 240);
static const uint16_t C_DIM = lgfx::color565(120, 132, 150);
static const uint16_t C_ACCENT = lgfx::color565(64, 228, 255);
static const uint16_t C_GREEN = lgfx::color565(80, 220, 120);
static const uint16_t C_RED = lgfx::color565(255, 90, 90);
static const uint16_t C_AMBER = lgfx::color565(255, 190, 70);
static const uint16_t C_SEL = lgfx::color565(32, 70, 96);

static M5Canvas spr(&M5Cardputer.Display);
static Preferences prefs;

// ---- ajustes -----------------------------------------------------------------------------------------
struct Settings {
    char long_name[40];
    char short_name[5];
    uint8_t region;       // REGIONS[]
    uint8_t preset;       // PRESETS[]
    char channel[12];     // "" = el nombre del preset (canal público)
    uint8_t psk[32];
    uint8_t psk_len;
    uint16_t slot;        // 0 = por nombre de canal
    int8_t power;         // dBm
    uint8_t hops;
    bool relay, gps, share_pos, sound;
};
static Settings S;
static bool okMqtt = true;   // aparte de Settings: agregarlo ahí invalidaría la config guardada
static bool wifiMqtt = true; // gateway propio: publicar en MQTT por WiFi
static uint32_t myNode;

static void defaultSettings() {
    memset(&S, 0, sizeof(S));
    snprintf(S.long_name, sizeof(S.long_name), "Cardputer %04x", (unsigned)(myNode & 0xFFFF));
    snprintf(S.short_name, sizeof(S.short_name), "%04x", (unsigned)(myNode & 0xFFFF));
    S.region = 0;   // ANZ: Argentina
    S.preset = 0;   // LongFast
    S.psk[0] = 1;   // clave pública por defecto ("AQ==")
    S.psk_len = 1;
    S.power = 22;
    S.hops = 3;
    S.relay = true;
    S.gps = true;
    S.share_pos = false; // la posición se comparte solo si el usuario lo pide
    S.sound = true;
}

static void loadSettings() {
    prefs.begin("meshtastic", false);
    myNode = prefs.getUInt("node", 0);
    if (myNode < 0x10 || myNode == BROADCAST) {
        do myNode = esp_random(); while (myNode < 0x10 || myNode == BROADCAST);
        prefs.putUInt("node", myNode);
    }
    defaultSettings();
    Settings tmp;
    if (prefs.getBytes("cfg", &tmp, sizeof(tmp)) == sizeof(tmp)) S = tmp;
    if (S.region >= NUM_REGIONS) S.region = 0;
    if (S.preset >= NUM_PRESETS) S.preset = 0;
    if (S.hops < 1 || S.hops > 7) S.hops = 3;
    if (S.power < 2 || S.power > 22) S.power = 22;
    S.long_name[sizeof(S.long_name) - 1] = 0;
    S.short_name[sizeof(S.short_name) - 1] = 0;
    S.channel[sizeof(S.channel) - 1] = 0;
    if (S.psk_len > 32) S.psk_len = 1;
    okMqtt = prefs.getBool("mqtt", true);
    wifiMqtt = prefs.getBool("wifimqtt", true);
}

static void saveSettings() { prefs.putBytes("cfg", &S, sizeof(S)); }

// ---- radio --------------------------------------------------------------------------------------------
// El router y los mensajes (~30 KB) van al heap, reservados después del canvas: así el canvas de
// pantalla completa (64 KB contiguos) se reserva primero, antes de que el heap se fragmente.
static Router *g_router;
#define R (*g_router)
static bool radioUp = false;
static int radioErr = 0;

static uint32_t p_now() { return millis(); }
static uint32_t p_rnd() { return esp_random(); }
static int p_aes(const uint8_t *key, int bits, const uint8_t in[16], uint8_t out[16]) {
    return cpos_api->crypto.aes_ecb(key, bits, 1, in, out);
}
static int p_send(const uint8_t *p, size_t n) { return radioUp ? cpos_api->lora.send(p, n) : 0; } // sin radio: se descarta
static int p_busy() { return radioUp && (cpos_api->lora.busy() || cpos_api->lora.pending() > 1); }

static const Region &region() { return REGIONS[S.region]; }
static const Preset &preset() { return PRESETS[S.preset]; }
static const char *chanName() { return S.channel[0] ? S.channel : preset().name; }
static uint32_t freqHz() { return frequency_hz(region(), preset(), chanName(), S.slot); }

static cpos_lora_cfg_t radioCfg() {
    cpos_lora_cfg_t c = {};
    c.freq_hz = freqHz();
    c.bw_hz = preset().bw_hz;
    c.sf = preset().sf;
    c.cr = preset().cr;
    c.sync_word = SYNC_WORD;
    c.power_dbm = S.power > region().max_power ? region().max_power : S.power;
    c.preamble = PREAMBLE;
    c.crc = 1;
    return c;
}

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
}

static char radioDiag[220];

static void startRadio() {
    cpos_lora_cfg_t c = radioCfg();
    radioErr = cpos_api->lora.begin(&c);
    radioUp = radioErr == 0;
    radioDiag[0] = 0;
    if (cpos_api->abi_minor >= 6 && cpos_api->lora.diag) cpos_api->lora.diag(radioDiag, sizeof(radioDiag));
}

static void reconfigureRadio() {
    applyRouter();
    if (radioUp) {
        cpos_lora_cfg_t c = radioCfg();
        cpos_api->lora.configure(&c);
    } else {
        startRadio();
    }
}

// ---- GNSS (NMEA) --------------------------------------------------------------------------------------
struct Gnss {
    bool up = false, fix = false;
    int32_t lat = 0, lon = 0; // grados * 1e7
    int32_t alt = 0;
    uint8_t sats = 0;
    uint32_t unix = 0, unix_at = 0; // hora UTC y millis() en que se leyó
    uint32_t fix_at = 0;
    char line[100];
    uint8_t n = 0;
} G;

static int32_t nmeaCoord(const char *v, const char *hemi) {
    // ddmm.mmmm / dddmm.mmmm -> grados * 1e7
    if (!*v) return 0;
    double x = atof(v);
    int deg = (int)(x / 100);
    double minutes = x - deg * 100;
    double d = deg + minutes / 60.0;
    if (*hemi == 'S' || *hemi == 'W') d = -d;
    return (int32_t)lround(d * 1e7);
}

static uint32_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468);
}

static void nmeaLine(char *s) {
    // checksum
    char *star = strchr(s, '*');
    if (!star || s[0] != '$') return;
    uint8_t cs = 0;
    for (char *p = s + 1; p < star; p++) cs ^= (uint8_t)*p;
    if (strtoul(star + 1, nullptr, 16) != cs) return;
    *star = 0;
    char *f[20];
    int nf = 0;
    for (char *p = s; nf < 20;) {
        f[nf++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    const char *type = f[0] + 3; // $GPRMC / $GNRMC ...
    if (!strcmp(type, "RMC") && nf >= 10) {
        bool valid = f[2][0] == 'A';
        if (valid) {
            G.lat = nmeaCoord(f[3], f[4]);
            G.lon = nmeaCoord(f[5], f[6]);
            G.fix = true;
            G.fix_at = millis();
        }
        if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
            int hh = (f[1][0] - '0') * 10 + f[1][1] - '0', mi = (f[1][2] - '0') * 10 + f[1][3] - '0',
                ss = (f[1][4] - '0') * 10 + f[1][5] - '0';
            int dd = (f[9][0] - '0') * 10 + f[9][1] - '0', mo = (f[9][2] - '0') * 10 + f[9][3] - '0',
                yy = 2000 + (f[9][4] - '0') * 10 + f[9][5] - '0';
            if (yy >= 2024 && mo >= 1 && mo <= 12) {
                G.unix = daysFromCivil(yy, mo, dd) * 86400u + hh * 3600 + mi * 60 + ss;
                G.unix_at = millis();
            }
        }
    } else if (!strcmp(type, "GGA") && nf >= 10) {
        G.sats = (uint8_t)atoi(f[7]);
        if (atoi(f[6]) > 0) G.alt = (int32_t)lround(atof(f[9]));
    }
}

static void pollGnss() {
    if (!G.up) return;
    uint8_t buf[128];
    int n;
    while ((n = cpos_api->lora.gnss_read(buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (G.n) { G.line[G.n] = 0; nmeaLine(G.line); }
                G.n = 0;
            } else if (G.n < sizeof(G.line) - 1) {
                G.line[G.n++] = c;
            } else {
                G.n = 0;
            }
        }
    }
    if (G.fix && millis() - G.fix_at > 10000) G.fix = false; // sin datos válidos hace rato
}

static void setGps(bool on) {
    if (on && !G.up) G.up = cpos_api->lora.gnss_begin(115200) == 0;
    if (!on && G.up) { cpos_api->lora.gnss_end(); G.up = false; G.fix = false; }
}

static uint32_t nowUnix() { return G.unix ? G.unix + (millis() - G.unix_at) / 1000 : 0; }

// ---- mensajes -------------------------------------------------------------------------------------------
enum MsgStatus : uint8_t { ST_NONE, ST_SENDING, ST_OK, ST_FAIL, ST_RELAYED };
struct Msg {
    uint32_t from, to, id;
    uint32_t at;       // millis() (0 = de una sesión anterior)
    uint8_t status;
    int8_t snr;
    uint8_t hops;
    char text[MAX_TEXT + 1];
};
static constexpr int MAX_MSGS = 40;
static Msg *msgs;
static int msgCount = 0;
static bool msgsDirty = false, nodesDirty = false;

// conversación abierta: BROADCAST = canal, si no el número del nodo
static uint32_t conv = BROADCAST;
struct Unread { uint32_t conv; uint16_t n; };
static Unread unread[16];

static void addUnread(uint32_t c) {
    for (auto &u : unread) if (u.n && u.conv == c) { u.n++; return; }
    for (auto &u : unread) if (!u.n) { u.conv = c; u.n = 1; return; }
}
static int unreadFor(uint32_t c) {
    for (auto &u : unread) if (u.n && u.conv == c) return u.n;
    return 0;
}
static void clearUnread(uint32_t c) {
    for (auto &u : unread) if (u.conv == c) u.n = 0;
}
static int unreadTotal() {
    int t = 0;
    for (auto &u : unread) t += u.n;
    return t;
}

static Msg *addMsg(uint32_t from, uint32_t to, uint32_t id, const char *text, size_t len) {
    if (msgCount == MAX_MSGS) {
        memmove(&msgs[0], &msgs[1], sizeof(Msg) * (MAX_MSGS - 1));
        msgCount--;
    }
    Msg &m = msgs[msgCount++];
    memset(&m, 0, sizeof(m));
    m.from = from;
    m.to = to;
    m.id = id;
    m.at = millis() | 1;
    if (len > MAX_TEXT) len = MAX_TEXT;
    memcpy(m.text, text, len);
    m.text[len] = 0;
    msgsDirty = true;
    return &m;
}

static bool inConv(const Msg &m, uint32_t c) {
    if (c == BROADCAST) return m.to == BROADCAST;
    return m.to != BROADCAST && (m.from == c || m.to == c);
}

static uint32_t convOf(const Msg &m) { return m.to == BROADCAST ? BROADCAST : (m.from == myNode ? m.to : m.from); }

// ---- SD ---------------------------------------------------------------------------------------------------
static const char *STORE_DIR = "/sdcard/meshtastic";
static const uint32_t FILE_MAGIC = 0x4D534831; // "MSH1"
static bool sdOk = false;

static void saveFile(const char *name, const void *data, uint32_t itemSize, uint32_t count) {
    if (!sdOk) return;
    char path[64], tmp[64];
    snprintf(path, sizeof(path), "%s/%s", STORE_DIR, name);
    snprintf(tmp, sizeof(tmp), "%s/%s.tmp", STORE_DIR, name);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint32_t h[3] = {FILE_MAGIC, itemSize, count};
    bool ok = fwrite(h, sizeof(h), 1, f) == 1 && (count == 0 || fwrite(data, itemSize, count, f) == count);
    fclose(f);
    if (ok) { remove(path); rename(tmp, path); }
}

static uint32_t loadFile(const char *name, void *data, uint32_t itemSize, uint32_t max) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", STORE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t h[3] = {0};
    uint32_t n = 0;
    if (fread(h, sizeof(h), 1, f) == 1 && h[0] == FILE_MAGIC && h[1] == itemSize) {
        n = h[2] > max ? max : h[2];
        n = (uint32_t)fread(data, itemSize, n, f);
    }
    fclose(f);
    return n;
}

static void loadStore() {
    mkdir(STORE_DIR, 0777);
    FILE *probe = fopen("/sdcard/meshtastic/.probe", "wb");
    sdOk = probe != nullptr;
    if (probe) { fclose(probe); remove("/sdcard/meshtastic/.probe"); }
    if (!sdOk) return;
    msgCount = (int)loadFile("msgs.bin", msgs, sizeof(Msg), MAX_MSGS);
    for (int i = 0; i < msgCount; i++) {
        msgs[i].at = 0;
        msgs[i].text[MAX_TEXT] = 0;
        if (msgs[i].status == ST_SENDING) msgs[i].status = ST_FAIL;
    }
    R.num_nodes = (int)loadFile("nodes.bin", R.nodes, sizeof(Node), Router::MAX_NODES);
    for (int i = 0; i < R.num_nodes; i++) R.nodes[i].last_heard = 0;
}

static uint32_t lastSave = 0;
static void saveStore(bool force = false) {
    if (!sdOk || (!force && millis() - lastSave < 10000)) return;
    if (msgsDirty) saveFile("msgs.bin", msgs, sizeof(Msg), msgCount);
    if (nodesDirty) saveFile("nodes.bin", R.nodes, sizeof(Node), R.num_nodes);
    msgsDirty = nodesDirty = false;
    lastSave = millis();
}

// ---- eventos del router ------------------------------------------------------------------------------------
static bool needDraw = true;

static void beep() {
    if (!S.sound) return;
    static bool spk = false;
    if (!spk) spk = M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.tone(2200, 50, 0, true);
}

struct App : Listener {
    void on_text(const Header &h, const Data &d, const Node *) override {
        Msg *m = addMsg(h.from, h.to, h.id, (const char *)d.payload, d.payload_len);
        m->hops = h.hop_start() >= h.hop_limit() ? h.hop_start() - h.hop_limit() : 0;
        uint32_t c = convOf(*m);
        if (c != conv) addUnread(c);
        beep();
        needDraw = true;
    }
    void on_ack(uint32_t id, bool ok, uint32_t) override {
        for (int i = msgCount - 1; i >= 0; i--)
            if (msgs[i].id == id && msgs[i].from == myNode) {
                msgs[i].status = ok ? (msgs[i].to == BROADCAST ? ST_RELAYED : ST_OK) : ST_FAIL;
                msgsDirty = true;
                needDraw = true;
                break;
            }
    }
    void on_node(const Node &) override {
        nodesDirty = true;
        needDraw = true;
    }
} app;

static void sendText(const char *text) {
    size_t len = strlen(text);
    if (!len) return;
    uint32_t id = R.send_text(conv, text);
    Msg *m = addMsg(myNode, conv, id, text, len);
    // un DM espera el ack; un broadcast queda "enviado" hasta que alguien lo retransmita
    if (!id || !radioUp) m->status = ST_FAIL;
    else m->status = conv == BROADCAST ? ST_NONE : ST_SENDING;
}

static Position myPosition() {
    Position p;
    if (!G.fix) return p;
    p.has_pos = true;
    p.latitude_i = G.lat;
    p.longitude_i = G.lon;
    p.altitude = G.alt;
    p.time = nowUnix();
    p.location_source = 2; // GPS interno
    p.sats_in_view = G.sats;
    p.precision_bits = 32;
    if (psk_is_public(S.psk, S.psk_len)) { // canal público: posición aproximada, como el firmware
        reduce_precision(p.latitude_i, MAX_PUBLIC_PRECISION);
        reduce_precision(p.longitude_i, MAX_PUBLIC_PRECISION);
        p.precision_bits = MAX_PUBLIC_PRECISION;
    }
    return p;
}

// ---- MQTT por WiFi: este nodo es su propio gateway ---------------------------------------------------------
// Publica en mqtt.meshtastic.org lo mismo que un nodo oficial con MQTT: sus paquetes (cifrados, en
// msh/<región>/2/e/<canal>/<id>), los de otros que lo permiten, y un "map report" con la posición
// aproximada. Así aparece en los mapas públicos aunque no haya otro gateway cerca. La conexión corre en
// una tarea propia para no trabar la pantalla.
struct UpMsg { char topic[72]; uint16_t len; uint8_t data[400]; };
static constexpr int UPQ = 6;
static UpMsg *upq;
static int upHead = 0, upN = 0;
static void *upLock;
enum { MQ_OFF, MQ_NO_WIFI, MQ_CONNECTING, MQ_UP };
static volatile int mqttState = MQ_OFF;
static volatile uint32_t mqttSent = 0;
static bool mqttStarted = false;

static void nodeId(char *b, size_t n) { snprintf(b, n, "!%08x", (unsigned)myNode); }
static void topicRoot(char *b, size_t n) { snprintf(b, n, "msh/%s/2", region().name); }

static void upPush(const char *topic, const uint8_t *d, size_t n) {
    if (!upq || !n || n > sizeof(upq->data)) return;
    if (cpos_api->rtos.mutex_lock(upLock, 100)) return;
    if (upN == UPQ) { upHead = (upHead + 1) % UPQ; upN--; } // sin conexión: se pierde el más viejo
    UpMsg &m = upq[(upHead + upN) % UPQ];
    strncpy(m.topic, topic, sizeof(m.topic) - 1);
    m.topic[sizeof(m.topic) - 1] = 0;
    m.len = (uint16_t)n;
    memcpy(m.data, d, n);
    upN++;
    cpos_api->rtos.mutex_unlock(upLock);
}

static bool upPop(UpMsg &m) {
    bool got = false;
    if (cpos_api->rtos.mutex_lock(upLock, 100)) return false;
    if (upN) { m = upq[upHead]; upHead = (upHead + 1) % UPQ; upN--; got = true; }
    cpos_api->rtos.mutex_unlock(upLock);
    return got;
}

static void p_uplink(const uint8_t *p, size_t n, int16_t rssi, float snr) {
    if (!wifiMqtt || !upq) return;
    static uint8_t env[400];
    char gw[12], root[24], topic[72];
    nodeId(gw, sizeof(gw));
    topicRoot(root, sizeof(root));
    size_t en = encode_envelope(p, n, nowUnix(), rssi, snr, R.channel_name(), gw, env, sizeof(env));
    snprintf(topic, sizeof(topic), "%s/e/%s/%s", root, R.channel_name(), gw);
    upPush(topic, env, en);
}

static int sendAll(int fd, const uint8_t *b, size_t n) {
    size_t off = 0;
    for (uint32_t t0 = millis(); off < n;) {
        int r = cpos_api->net.sock_send(fd, b + off, n - off, 0);
        if (r > 0) off += r;
        else if (millis() - t0 > 5000) return -1;
        else delay(20);
    }
    return 0;
}

static void mqttTask(void *) {
    static uint8_t buf[480];
    static UpMsg m;
    for (;;) {
        if (!wifiMqtt) { mqttState = MQ_OFF; delay(1000); continue; }
        if (cpos_api->net.status() != 3) { // la red guardada en System > WiFi
            mqttState = MQ_NO_WIFI;
            cpos_api->net.connect("", "");
            delay(3000);
            continue;
        }
        mqttState = MQ_CONNECTING;
        uint32_t ip = 0;
        if (cpos_api->net.resolve(mqtt::BROKER, &ip)) { delay(10000); continue; }
        int fd = cpos_api->net.socket(2, 1, 0);
        if (fd < 0) { delay(10000); continue; }
        cpos_sockaddr_in a = {sizeof(a), 2, (uint16_t)((mqtt::PORT & 0xFF) << 8 | mqtt::PORT >> 8), ip, {0}};
        if (cpos_api->net.sock_connect(fd, &a, sizeof(a)) != 0) { cpos_api->net.close(fd); delay(15000); continue; }
        cpos_api->net.fcntl(fd, 4 /*F_SETFL*/, 1 /*O_NONBLOCK*/);
        char cid[12];
        nodeId(cid, sizeof(cid));
        size_t n = mqtt::connect(cid, mqtt::USER, mqtt::PASS, buf, sizeof(buf));
        bool ok = sendAll(fd, buf, n) == 0;
        uint8_t ack[4];
        int got = 0;
        for (uint32_t t0 = millis(); ok && got < 4 && millis() - t0 < 8000;) {
            int r = cpos_api->net.sock_recv(fd, ack + got, 4 - got, 0);
            if (r > 0) got += r;
            else if (r == 0) ok = false;
            else delay(50);
        }
        if (!ok || !mqtt::connack_ok(ack, got)) { cpos_api->net.close(fd); delay(15000); continue; }
        mqttState = MQ_UP;
        uint32_t lastPing = millis(), lastRx = millis();
        while (wifiMqtt && cpos_api->net.status() == 3) {
            bool err = false;
            while (upPop(m)) {
                n = mqtt::publish(m.topic, m.data, m.len, buf, sizeof(buf));
                if (!n || sendAll(fd, buf, n)) { err = true; break; }
                mqttSent++;
            }
            if (err) break;
            if (millis() - lastPing > 30000) {
                n = mqtt::pingreq(buf);
                if (sendAll(fd, buf, n)) break;
                lastPing = millis();
            }
            uint8_t rx[64];
            int r = cpos_api->net.sock_recv(fd, rx, sizeof(rx), 0);
            if (r > 0) lastRx = millis();
            else if (r == 0) break;                  // el broker cerró
            if (millis() - lastRx > 100000) break;   // sin respuesta a los ping
            delay(100);
        }
        cpos_api->net.close(fd);
        mqttState = MQ_CONNECTING;
        delay(5000);
    }
}

static void startMqtt() {
    if (mqttStarted) return;
    if (cpos_api->abi_minor < 7) return; // OS sin DNS
    upq = (UpMsg *)calloc(UPQ, sizeof(UpMsg));
    upLock = cpos_api->rtos.mutex_create(0);
    if (!upq || !upLock) return;
    mqttStarted = cpos_api->rtos.task_create(mqttTask, "mqtt", 6144, nullptr, 3) >= 0;
}

static void sendMapReport() {
    if (!wifiMqtt || !upq || !G.fix) return;
    Position pos = myPosition();
    MapReport r{S.long_name, S.short_name, HW_M5STACK_CARDPUTER_ADV, region().code, preset().code, "cardputeros-" APP_VERSION,
                !S.channel[0] && psk_is_public(S.psk, S.psk_len), pos.latitude_i, pos.longitude_i, pos.altitude, 0};
    for (int i = 0; i < R.num_nodes; i++)
        if (R.nodes[i].last_heard && millis() - R.nodes[i].last_heard < 2u * 3600 * 1000) r.num_online_local_nodes++;
    static uint8_t env[400];
    char gw[12], root[24], topic[72];
    nodeId(gw, sizeof(gw));
    topicRoot(root, sizeof(root));
    size_t n = encode_map_report(myNode, r, R.channel_name(), gw, env, sizeof(env));
    snprintf(topic, sizeof(topic), "%s/map/", root);
    upPush(topic, env, n);
}

// ---- tareas periódicas ------------------------------------------------------------------------------------
static uint32_t nextNodeInfo = 15000, nextPosition = 60000, nextTelemetry = 120000;
static cpos_lora_stats_t lastStats;
static uint32_t lastStatsAt = 0;

static void periodic() {
    uint32_t t = millis();
    // al conectarse a MQTT, anunciarse y (si se comparte) mandar la posición enseguida
    static int lastMq = MQ_OFF;
    if (mqttState == MQ_UP && lastMq != MQ_UP) {
        nextNodeInfo = t + 2000;
        if (S.share_pos) nextPosition = t + 5000;
        needDraw = true;
    }
    if (mqttState != lastMq) needDraw = true;
    lastMq = mqttState;
    if ((int32_t)(t - nextNodeInfo) >= 0) {
        if (radioUp || mqttState == MQ_UP) R.send_nodeinfo();
        nextNodeInfo = t + 3u * 3600 * 1000; // cada 3 h, como el firmware
    }
    if ((int32_t)(t - nextPosition) >= 0) {
        if ((radioUp || mqttState == MQ_UP) && S.share_pos && G.fix) {
            R.send_position(myPosition());
            if (okMqtt) sendMapReport();
            nextPosition = t + 15u * 60 * 1000;
        } else {
            nextPosition = t + 60000;
        }
    }
    if ((int32_t)(t - nextTelemetry) >= 0) {
        nextTelemetry = t + 30u * 60 * 1000;
        if (radioUp || mqttState == MQ_UP) {
            cpos_lora_stats_t s = {};
            cpos_api->lora.stats(&s);
            uint32_t span = t - lastStatsAt;
            DeviceMetrics m;
            m.battery_level = (uint32_t)M5Cardputer.Power.getBatteryLevel();
            m.voltage = M5Cardputer.Power.getBatteryVoltage() / 1000.0f;
            if (span) {
                m.air_util_tx = 100.0f * (s.tx_air_ms - lastStats.tx_air_ms) / span;
                m.channel_utilization = 100.0f * (s.tx_air_ms + s.rx_air_ms - lastStats.tx_air_ms - lastStats.rx_air_ms) / span;
            }
            m.uptime_seconds = t / 1000;
            R.send_telemetry(nowUnix(), m);
            lastStats = s;
            lastStatsAt = t;
        }
    }
}

// ---- texto ------------------------------------------------------------------------------------------------
static const lgfx::IFont *FONT = &fonts::lgfxJapanGothicP_12;
static const int LINE_H = 13;

static size_t utf8Len(uint8_t c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

// corta s en líneas de ancho w; devuelve la cantidad (como mucho max) y los comienzos en starts/lens
static int wrap(const char *s, int w, const char **starts, int *lens, int max) {
    int n = 0;
    const char *p = s;
    char tmp[MAX_TEXT + 1];
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *best = nullptr;
        const char *q = p;
        while (*q) {
            const char *nq = q + utf8Len((uint8_t)*q);
            size_t l = nq - p;
            memcpy(tmp, p, l);
            tmp[l] = 0;
            if (spr.textWidth(tmp) > w) break;
            if (*nq == ' ' || *nq == 0) best = nq;
            q = nq;
            if (*q == '\n') break;
        }
        const char *end = *q == 0 ? q : (best ? best : (q > p ? q : p + utf8Len((uint8_t)*p)));
        if (*q == '\n') end = q;
        starts[n] = p;
        lens[n] = (int)(end - p);
        n++;
        p = end;
        if (*p == '\n') p++;
    }
    return n;
}

static void drawClipped(const char *s, int x, int y, int w) {
    char b[64];
    strncpy(b, s, sizeof(b) - 1);
    b[sizeof(b) - 1] = 0;
    size_t l = strlen(b);
    while (l && spr.textWidth(b) > w) {
        do l--; while (l && (b[l] & 0xC0) == 0x80);
        b[l] = 0;
    }
    spr.drawString(b, x, y);
}

static void ageText(uint32_t at, char *out, size_t n) {
    if (!at) { snprintf(out, n, "-"); return; }
    uint32_t s = (millis() - at) / 1000;
    if (s < 60) snprintf(out, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
    else if (s < 86400) snprintf(out, n, "%uh", (unsigned)(s / 3600));
    else snprintf(out, n, "%ud", (unsigned)(s / 86400));
}

static const char *nodeShort(uint32_t num, char *buf, size_t n) {
    if (num == myNode) return S.short_name;
    Node *nd = R.find(num);
    if (nd && nd->has_user && nd->user.short_name[0]) return nd->user.short_name;
    snprintf(buf, n, "%04x", (unsigned)(num & 0xFFFF));
    return buf;
}

static const char *nodeLong(uint32_t num, char *buf, size_t n) {
    Node *nd = R.find(num);
    if (nd && nd->has_user && nd->user.long_name[0]) return nd->user.long_name;
    snprintf(buf, n, "!%08x", (unsigned)num);
    return buf;
}

static uint16_t nodeColor(uint32_t num) {
    static const uint16_t pal[] = {lgfx::color565(255, 140, 90), lgfx::color565(120, 200, 255), lgfx::color565(170, 140, 255),
                                   lgfx::color565(110, 230, 160), lgfx::color565(255, 120, 190), lgfx::color565(240, 220, 90)};
    return pal[(num ^ (num >> 8) ^ (num >> 16)) % 6];
}

static double distanceKm(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2, double *bearing) {
    double a1 = lat1 * 1e-7 * M_PI / 180, a2 = lat2 * 1e-7 * M_PI / 180;
    double dl = (lon2 - lon1) * 1e-7 * M_PI / 180;
    double x = sin((a2 - a1) / 2), y = sin(dl / 2);
    double h = x * x + cos(a1) * cos(a2) * y * y;
    if (bearing) {
        double b = atan2(sin(dl) * cos(a2), cos(a1) * sin(a2) - sin(a1) * cos(a2) * cos(dl)) * 180 / M_PI;
        *bearing = fmod(b + 360, 360);
    }
    return 2 * 6371.0 * asin(sqrt(h));
}

// ---- pantallas ---------------------------------------------------------------------------------------------
enum Screen { SCR_CHAT, SCR_NODES, SCR_CONFIG };
static Screen screen = SCR_CHAT;
static char input[MAX_TEXT + 1];
static int inputLen = 0;
static int chatScroll = 0; // líneas desde abajo
static int nodeSel = 0, nodeTop = 0;
static bool nodeDetail = false;
static int cfgSel = 0, cfgTop = 0;
static bool cfgEditing = false;
static char cfgBuf[48];
static int cfgBufLen = 0;
static char toast[48];
static uint32_t toastUntil = 0;

static void showToast(const char *t) {
    strncpy(toast, t, sizeof(toast) - 1);
    toast[sizeof(toast) - 1] = 0;
    toastUntil = millis() + 2000;
    needDraw = true;
}

static void drawTopBar(const char *title, uint16_t titleColor) {
    spr.fillRect(0, 0, 240, 14, C_BAR);
    spr.setFont(FONT);
    spr.setTextColor(titleColor);
    drawClipped(title, 3, 1, 130);
    // derecha: radio, GPS, batería
    char b[24];
    int x = 237;
    snprintf(b, sizeof(b), "%d%%", (int)M5Cardputer.Power.getBatteryLevel());
    spr.setTextColor(C_DIM);
    x -= spr.textWidth(b);
    spr.drawString(b, x, 1);
    if (G.up) {
        snprintf(b, sizeof(b), G.fix ? "GPS%u" : "GPS", G.sats);
        spr.setTextColor(G.fix ? C_GREEN : C_DIM);
        x -= spr.textWidth(b) + 6;
        spr.drawString(b, x, 1);
    }
    if (wifiMqtt && mqttStarted) {
        const char *mq = "MQTT";
        spr.setTextColor(mqttState == MQ_UP ? C_GREEN : C_DIM);
        x -= spr.textWidth(mq) + 6;
        spr.drawString(mq, x, 1);
    }
    const char *r = radioUp ? "LoRa" : "sin radio";
    spr.setTextColor(radioUp ? (cpos_api->lora.busy() ? C_AMBER : C_GREEN) : C_RED);
    x -= spr.textWidth(r) + 6;
    spr.drawString(r, x, 1);
    // pestañas
    int u = unreadTotal();
    if (u && screen != SCR_CHAT) {
        snprintf(b, sizeof(b), "%d", u);
        spr.fillRoundRect(x - spr.textWidth(b) - 10, 2, spr.textWidth(b) + 6, 11, 3, C_ACCENT);
        spr.setTextColor(C_BG);
        spr.drawString(b, x - spr.textWidth(b) - 7, 1);
    }
}

static void convTitle(char *out, size_t n) {
    char tmp[24];
    if (conv == BROADCAST) snprintf(out, n, "# %s", R.channel_name());
    else snprintf(out, n, "@ %s", nodeShort(conv, tmp, sizeof(tmp)));
}

static void drawChat() {
    char title[40];
    convTitle(title, sizeof(title));
    spr.setFont(FONT);

    // líneas de la conversación, de abajo hacia arriba
    const int top = 15, bottom = 118;
    int y = bottom - chatScroll * LINE_H;
    bool any = false;
    for (int i = msgCount - 1; i >= 0 && y > top - 400; i--) {
        const Msg &m = msgs[i];
        if (!inConv(m, conv)) continue;
        any = true;
        const char *st[8];
        int ln[8];
        int n = wrap(m.text, 230, st, ln, 8);
        int h = (n + 1) * LINE_H;
        int y0 = y - h;
        // encabezado: quién, cuándo, estado
        char who[24], age[12], hdr[64];
        bool mine = m.from == myNode;
        ageText(m.at, age, sizeof(age));
        if (y0 + LINE_H > top && y0 < bottom) {
            int x = 3;
            spr.setTextColor(mine ? C_ACCENT : nodeColor(m.from));
            x += spr.drawString(nodeShort(m.from, who, sizeof(who)), x, y0);
            snprintf(hdr, sizeof(hdr), "  %s", age);
            if (!mine && m.hops) snprintf(hdr + strlen(hdr), sizeof(hdr) - strlen(hdr), " · %u salto%s", m.hops, m.hops > 1 ? "s" : "");
            spr.setTextColor(C_DIM);
            x += spr.drawString(hdr, x, y0);
            if (mine) {
                const char *s = m.status == ST_OK ? " ✓" : m.status == ST_RELAYED ? " ✓ retransmitido" : m.status == ST_FAIL ? " sin entrega" : m.status == ST_SENDING ? " …" : "";
                spr.setTextColor(m.status == ST_FAIL ? C_RED : C_GREEN);
                spr.drawString(s, x, y0);
            }
        }
        spr.setTextColor(C_TEXT);
        for (int k = 0; k < n; k++) {
            int ly = y0 + (k + 1) * LINE_H;
            if (ly + LINE_H <= top || ly >= bottom) continue;
            char b[MAX_TEXT + 1];
            memcpy(b, st[k], ln[k]);
            b[ln[k]] = 0;
            spr.drawString(b, 8, ly);
        }
        y = y0 - 2;
    }
    drawTopBar(title, C_ACCENT); // tapa lo que se pasó por arriba
    if (!any) {
        spr.setTextColor(C_DIM);
        spr.drawCenterString(conv == BROADCAST ? "Sin mensajes en el canal todavía" : "Sin mensajes con este nodo", 120, radioUp ? 50 : 22);
        char b[48];
        snprintf(b, sizeof(b), "%s %.3f MHz · %s", region().name, freqHz() / 1e6, preset().name);
        spr.drawCenterString(b, 120, radioUp ? 66 : 36);
    }
    if (!radioUp) {
        // diagnóstico del OS (expansor, BUSY, registro del SX1262, GPS), sobre los mensajes si los hay
        if (cpos_api->abi_minor >= 6 && cpos_api->lora.diag) cpos_api->lora.diag(radioDiag, sizeof(radioDiag));
        const char *st[4];
        int ln[4];
        int n = wrap(radioDiag, 228, st, ln, 4);
        int y0 = 117 - (n + 2) * LINE_H;
        spr.fillRoundRect(2, y0 - 2, 236, (n + 2) * LINE_H + 3, 3, C_BAR);
        spr.setTextColor(C_RED);
        spr.drawCenterString(radioErr == -2 ? "No responde el Cap LoRa-1262" : "Radio no disponible", 120, y0);
        spr.setTextColor(C_DIM);
        for (int k = 0; k < n; k++) {
            char b[MAX_TEXT + 1];
            memcpy(b, st[k], ln[k]);
            b[ln[k]] = 0;
            spr.drawCenterString(b, 120, y0 + (k + 1) * LINE_H);
        }
        spr.setTextColor(C_ACCENT);
        spr.drawCenterString("Enter: reintentar", 120, y0 + (n + 1) * LINE_H);
    }
    // entrada
    spr.fillRect(0, 120, 240, 15, C_BAR);
    spr.setTextColor(C_ACCENT);
    spr.drawString(">", 3, 121);
    spr.setTextColor(C_TEXT);
    const char *shown = input;
    while (spr.textWidth(shown) > 200) shown++;
    int w = spr.drawString(shown, 12, 121);
    if ((millis() / 500) & 1) spr.fillRect(13 + w, 122, 1, 11, C_ACCENT);
    char cnt[8];
    snprintf(cnt, sizeof(cnt), "%d", (int)(MAX_TEXT - inputLen));
    spr.setTextColor(C_DIM);
    spr.drawString(cnt, 237 - spr.textWidth(cnt), 121);
}

// orden de la lista de nodos: los oídos más recientemente primero
static int nodeOrder[Router::MAX_NODES];
static int sortedNodes() {
    int n = R.num_nodes;
    for (int i = 0; i < n; i++) nodeOrder[i] = i;
    for (int i = 1; i < n; i++) {
        int v = nodeOrder[i], j = i - 1;
        uint32_t lv = R.nodes[v].last_heard;
        while (j >= 0) {
            uint32_t lj = R.nodes[nodeOrder[j]].last_heard;
            bool before = lv && (!lj || (int32_t)(lv - lj) > 0);
            if (!before) break;
            nodeOrder[j + 1] = nodeOrder[j];
            j--;
        }
        nodeOrder[j + 1] = v;
    }
    return n;
}

static void drawNodeDetail(const Node &n) {
    spr.fillRoundRect(6, 18, 228, 114, 5, C_BAR);
    spr.drawRoundRect(6, 18, 228, 114, 5, C_ACCENT);
    char b[64], tmp[24];
    int y = 22;
    spr.setTextColor(nodeColor(n.num));
    drawClipped(nodeLong(n.num, tmp, sizeof(tmp)), 12, y, 216);
    y += LINE_H + 2;
    spr.setTextColor(C_TEXT);
    snprintf(b, sizeof(b), "!%08x · %s", (unsigned)n.num, n.has_user ? n.user.short_name : "?");
    spr.drawString(b, 12, y); y += LINE_H;
    char age[12];
    ageText(n.last_heard, age, sizeof(age));
    if (n.hops == 0) snprintf(b, sizeof(b), "Directo · SNR %.1f dB · %d dBm · %s", n.snr, n.rssi, age);
    else if (n.hops > 0) snprintf(b, sizeof(b), "%d salto%s · visto hace %s", n.hops, n.hops > 1 ? "s" : "", age);
    else snprintf(b, sizeof(b), "Visto hace %s", age);
    spr.drawString(b, 12, y); y += LINE_H;
    if (n.metrics.has) {
        if (n.metrics.battery_level > 100) snprintf(b, sizeof(b), "Batería: enchufado · %.2f V", n.metrics.voltage);
        else snprintf(b, sizeof(b), "Batería: %u%% · %.2f V · canal %.0f%%", (unsigned)n.metrics.battery_level, n.metrics.voltage, n.metrics.channel_utilization);
        spr.drawString(b, 12, y); y += LINE_H;
    }
    if (n.pos.has_pos) {
        snprintf(b, sizeof(b), "%.5f, %.5f · %d m", n.pos.latitude_i * 1e-7, n.pos.longitude_i * 1e-7, (int)n.pos.altitude);
        spr.drawString(b, 12, y); y += LINE_H;
        if (G.fix) {
            double brg;
            double km = distanceKm(G.lat, G.lon, n.pos.latitude_i, n.pos.longitude_i, &brg);
            static const char *dirs[] = {"N", "NE", "E", "SE", "S", "SO", "O", "NO"};
            if (km < 1) snprintf(b, sizeof(b), "A %d m hacia el %s", (int)(km * 1000), dirs[(int)((brg + 22.5) / 45) % 8]);
            else snprintf(b, sizeof(b), "A %.1f km hacia el %s", km, dirs[(int)((brg + 22.5) / 45) % 8]);
            spr.setTextColor(C_ACCENT);
            spr.drawString(b, 12, y); y += LINE_H;
        }
    } else {
        spr.setTextColor(C_DIM);
        spr.drawString("Sin posición", 12, y); y += LINE_H;
    }
    spr.setTextColor(C_DIM);
    spr.drawString("Enter: mensaje directo · Espacio: cerrar", 12, 117);
}

static void drawNodes() {
    char title[32];
    snprintf(title, sizeof(title), "Nodos (%d)", R.num_nodes);
    drawTopBar(title, C_ACCENT);
    spr.setFont(FONT);
    int n = sortedNodes();
    if (!n) {
        spr.setTextColor(C_DIM);
        spr.drawCenterString("Todavía no se escuchó a nadie", 120, 52);
        spr.drawCenterString("Los nodos aparecen al recibir sus paquetes", 120, 68);
        return;
    }
    if (nodeSel >= n) nodeSel = n - 1;
    const int rows = 9;
    if (nodeSel < nodeTop) nodeTop = nodeSel;
    if (nodeSel >= nodeTop + rows) nodeTop = nodeSel - rows + 1;
    for (int r = 0; r < rows && nodeTop + r < n; r++) {
        const Node &nd = R.nodes[nodeOrder[nodeTop + r]];
        int y = 16 + r * LINE_H;
        bool sel = nodeTop + r == nodeSel;
        if (sel) spr.fillRect(0, y - 1, 240, LINE_H, C_SEL);
        char tmp[24], right[32], age[12];
        spr.fillRoundRect(2, y, 30, 11, 2, nodeColor(nd.num));
        spr.setTextColor(C_BG);
        drawClipped(nd.has_user ? nd.user.short_name : nodeShort(nd.num, tmp, sizeof(tmp)), 4, y - 1, 27);
        ageText(nd.last_heard, age, sizeof(age));
        if (nd.pos.has_pos && G.fix) {
            double km = distanceKm(G.lat, G.lon, nd.pos.latitude_i, nd.pos.longitude_i, nullptr);
            if (km < 1) snprintf(right, sizeof(right), "%dm %s", (int)(km * 1000), age);
            else snprintf(right, sizeof(right), "%.1fkm %s", km, age);
        } else if (nd.hops == 0 && nd.last_heard) {
            snprintf(right, sizeof(right), "%.0fdB %s", nd.snr, age);
        } else if (nd.hops > 0) {
            snprintf(right, sizeof(right), "%dh %s", nd.hops, age);
        } else {
            snprintf(right, sizeof(right), "%s", age);
        }
        int rw = spr.textWidth(right);
        spr.setTextColor(C_TEXT);
        drawClipped(nodeLong(nd.num, tmp, sizeof(tmp)), 36, y - 1, 200 - rw - 36);
        spr.setTextColor(nd.hops == 0 ? C_GREEN : C_DIM);
        spr.drawString(right, 237 - rw, y - 1);
        int u = unreadFor(nd.num);
        if (u) spr.fillCircle(198 - rw, y + 5, 3, C_ACCENT);
    }
    if (nodeDetail) drawNodeDetail(R.nodes[nodeOrder[nodeSel]]);
}

// ---- configuración ------------------------------------------------------------------------------------------
enum CfgItem { CF_LONG, CF_SHORT, CF_REGION, CF_PRESET, CF_CHANNEL, CF_PSK, CF_SLOT, CF_POWER, CF_HOPS,
               CF_RELAY, CF_GPS, CF_SHARE, CF_MQTT, CF_WIFI, CF_SOUND, CF_SEND_INFO, CF_SEND_POS, CF_CLEAR, CF_COUNT };
static const char *CFG_NAMES[CF_COUNT] = {"Nombre", "Nombre corto", "Región", "Modo (preset)", "Canal", "Clave (PSK)",
                                          "Slot de frecuencia", "Potencia", "Saltos máx.", "Retransmitir", "GPS",
                                          "Compartir posición", "Subir a MQTT/mapa", "Gateway WiFi", "Sonido", "Anunciar nodo ahora", "Enviar posición ahora",
                                          "Borrar mensajes"};

static void cfgValue(int i, char *b, size_t n) {
    switch (i) {
    case CF_LONG: snprintf(b, n, "%s", S.long_name); break;
    case CF_SHORT: snprintf(b, n, "%s", S.short_name); break;
    case CF_REGION: snprintf(b, n, "%s", region().name); break;
    case CF_PRESET: snprintf(b, n, "%s", preset().name); break;
    case CF_CHANNEL: snprintf(b, n, "%s", S.channel[0] ? S.channel : "(público)"); break;
    case CF_PSK:
        if (S.psk_len == 1 && S.psk[0] == 1) snprintf(b, n, "AQ== (pública)");
        else if (S.psk_len == 0 || (S.psk_len == 1 && S.psk[0] == 0)) snprintf(b, n, "sin cifrado");
        else b64_encode(S.psk, S.psk_len, b, n);
        break;
    case CF_SLOT:
        if (S.slot) snprintf(b, n, "%u · %.3f MHz", S.slot, freqHz() / 1e6);
        else snprintf(b, n, "auto · %.3f MHz", freqHz() / 1e6);
        break;
    case CF_POWER: snprintf(b, n, "%d dBm", S.power); break;
    case CF_HOPS: snprintf(b, n, "%u", S.hops); break;
    case CF_RELAY: snprintf(b, n, "%s", S.relay ? "sí" : "no"); break;
    case CF_GPS: snprintf(b, n, "%s", S.gps ? (G.fix ? "sí · fix" : "sí · buscando") : "no"); break;
    case CF_SHARE: snprintf(b, n, "%s", S.share_pos ? "sí" : "no"); break;
    case CF_MQTT: snprintf(b, n, "%s", okMqtt ? "sí" : "no"); break;
    case CF_WIFI:
        if (!wifiMqtt) snprintf(b, n, "no");
        else if (!mqttStarted) snprintf(b, n, "sí · OS sin soporte");
        else snprintf(b, n, "%s", mqttState == MQ_UP ? "sí · conectado" : mqttState == MQ_NO_WIFI ? "sí · sin WiFi" : "sí · conectando");
        break;
    case CF_SOUND: snprintf(b, n, "%s", S.sound ? "sí" : "no"); break;
    default: b[0] = 0;
    }
}

static bool cfgIsText(int i) { return i == CF_LONG || i == CF_SHORT || i == CF_CHANNEL || i == CF_PSK; }

static void drawConfig() {
    char title[48];
    snprintf(title, sizeof(title), "Config · !%08x", (unsigned)myNode);
    drawTopBar(title, C_ACCENT);
    spr.setFont(FONT);
    const int rows = 8;
    if (cfgSel < cfgTop) cfgTop = cfgSel;
    if (cfgSel >= cfgTop + rows) cfgTop = cfgSel - rows + 1;
    for (int r = 0; r < rows && cfgTop + r < CF_COUNT; r++) {
        int i = cfgTop + r;
        int y = 16 + r * LINE_H;
        bool sel = i == cfgSel;
        if (sel) spr.fillRect(0, y - 1, 240, LINE_H, C_SEL);
        spr.setTextColor(i >= CF_SEND_INFO ? C_ACCENT : C_DIM);
        spr.drawString(CFG_NAMES[i], 4, y - 1);
        char v[64];
        if (sel && cfgEditing) {
            snprintf(v, sizeof(v), "%s_", cfgBuf);
            spr.setTextColor(C_AMBER);
        } else {
            cfgValue(i, v, sizeof(v));
            spr.setTextColor(C_TEXT);
        }
        int w = spr.textWidth(v);
        if (w > 120) {
            drawClipped(v, 116, y - 1, 120);
        } else {
            spr.drawString(v, 236 - w, y - 1);
        }
    }
    // pie: qué hace la tecla
    spr.fillRect(0, 121, 240, 14, C_BAR);
    spr.setTextColor(C_DIM);
    const char *help = cfgEditing ? "Escribí y Enter para guardar"
                       : cfgIsText(cfgSel) ? "Enter: editar"
                       : cfgSel >= CF_SEND_INFO ? "Enter: ejecutar"
                       : "Fn+, Fn+/ o Enter: cambiar";
    spr.drawString(help, 4, 122);
    char f[24];
    snprintf(f, sizeof(f), "%s %.3f", region().name, freqHz() / 1e6);
    spr.drawString(f, 236 - spr.textWidth(f), 122);
}

static void cfgChange(int dir) {
    switch (cfgSel) {
    case CF_REGION: S.region = (S.region + NUM_REGIONS + dir) % NUM_REGIONS; S.slot = 0; break;
    case CF_PRESET: S.preset = (S.preset + NUM_PRESETS + dir) % NUM_PRESETS; S.slot = 0; break;
    case CF_SLOT: {
        int n = (int)num_slots(region(), preset());
        S.slot = (uint16_t)((S.slot + n + 1 + dir) % (n + 1));
        break;
    }
    case CF_POWER: S.power = S.power + dir < 2 ? 22 : S.power + dir > 22 ? 2 : S.power + dir; break;
    case CF_HOPS: S.hops = S.hops + dir < 1 ? 7 : S.hops + dir > 7 ? 1 : S.hops + dir; break;
    case CF_RELAY: S.relay = !S.relay; break;
    case CF_GPS: S.gps = !S.gps; setGps(S.gps); break;
    case CF_SHARE: S.share_pos = !S.share_pos; if (S.share_pos) nextPosition = millis() + 5000; break;
    case CF_MQTT: okMqtt = !okMqtt; prefs.putBool("mqtt", okMqtt); break;
    case CF_WIFI: wifiMqtt = !wifiMqtt; prefs.putBool("wifimqtt", wifiMqtt); if (wifiMqtt) startMqtt(); break;
    case CF_SOUND: S.sound = !S.sound; break;
    default: return;
    }
    saveSettings();
    reconfigureRadio();
    needDraw = true;
}

static void cfgCommitText() {
    switch (cfgSel) {
    case CF_LONG:
        if (cfgBufLen) strncpy(S.long_name, cfgBuf, sizeof(S.long_name) - 1);
        nextNodeInfo = millis() + 3000;
        break;
    case CF_SHORT:
        if (cfgBufLen) { strncpy(S.short_name, cfgBuf, sizeof(S.short_name) - 1); S.short_name[4] = 0; }
        nextNodeInfo = millis() + 3000;
        break;
    case CF_CHANNEL:
        strncpy(S.channel, cfgBuf, sizeof(S.channel) - 1);
        S.channel[sizeof(S.channel) - 1] = 0;
        break;
    case CF_PSK: {
        uint8_t k[32];
        int n = cfgBufLen ? b64_decode(cfgBuf, k, sizeof(k)) : 1;
        if (!cfgBufLen) k[0] = 1;
        if (n == 0 || n == 1 || n == 16 || n == 32) {
            memcpy(S.psk, k, n);
            S.psk_len = (uint8_t)n;
        } else {
            showToast("Clave inválida: base64 de 1, 16 o 32 bytes");
            return;
        }
        break;
    }
    default: break;
    }
    saveSettings();
    reconfigureRadio();
}

static void cfgAction() {
    switch (cfgSel) {
    case CF_SEND_INFO:
        if (radioUp || mqttState == MQ_UP) { R.send_nodeinfo(); showToast("Nodo anunciado"); }
        else showToast("Sin radio");
        break;
    case CF_SEND_POS:
        if (!G.fix) showToast("Sin posición GPS todavía");
        else if (radioUp || mqttState == MQ_UP) {
            R.send_position(myPosition());
            if (okMqtt) sendMapReport();
            showToast("Posición enviada");
        }
        break;
    case CF_CLEAR:
        msgCount = 0;
        memset(unread, 0, sizeof(unread));
        msgsDirty = true;
        saveStore(true);
        showToast("Mensajes borrados");
        break;
    default: break;
    }
}

// ---- teclado ------------------------------------------------------------------------------------------------
static char prevKeys[CPOS_KBD_MAX_KEYS + 1];
static bool prevEnter = false, prevDel = false, prevTab = false, prevSpace = false;

static void openConvWith(uint32_t c) {
    conv = c;
    clearUnread(c);
    chatScroll = 0;
    screen = SCR_CHAT;
}

// conversaciones: el canal y cada nodo con el que hay mensajes, la más reciente primero
static int convList(uint32_t *out, int max) {
    int n = 0;
    out[n++] = BROADCAST;
    for (int i = msgCount - 1; i >= 0 && n < max; i--) {
        uint32_t c = convOf(msgs[i]);
        bool dup = false;
        for (int k = 0; k < n; k++) if (out[k] == c) dup = true;
        if (!dup) out[n++] = c;
    }
    if (conv != BROADCAST) {
        bool has = false;
        for (int k = 0; k < n; k++) if (out[k] == conv) has = true;
        if (!has && n < max) out[n++] = conv;
    }
    return n;
}

static void onKey(char c, bool fn) {
    if (fn) {
        bool up = c == ';', down = c == '.', left = c == ',', right = c == '/';
        if (screen == SCR_CHAT) {
            if (up) chatScroll++;
            if (down && chatScroll > 0) chatScroll--;
            if (left || right) {
                uint32_t list[17];
                int n = convList(list, 17), cur = 0;
                for (int i = 0; i < n; i++) if (list[i] == conv) cur = i;
                openConvWith(list[(cur + n + (right ? 1 : -1)) % n]);
            }
        } else if (screen == SCR_NODES) {
            if (up && nodeSel > 0) nodeSel--;
            if (down && nodeSel < R.num_nodes - 1) nodeSel++;
        } else if (!cfgEditing) {
            if (up) cfgSel = (cfgSel + CF_COUNT - 1) % CF_COUNT;
            if (down) cfgSel = (cfgSel + 1) % CF_COUNT;
            if (left) cfgChange(-1);
            if (right) cfgChange(1);
        }
        needDraw = true;
        return;
    }
    if (c < 32 || c > 126) return;
    if (screen == SCR_CHAT) {
        if (inputLen < (int)MAX_TEXT) { input[inputLen++] = c; input[inputLen] = 0; }
    } else if (screen == SCR_CONFIG && cfgEditing) {
        int max = cfgSel == CF_SHORT ? 4 : cfgSel == CF_CHANNEL ? 11 : cfgSel == CF_PSK ? 44 : 39;
        if (cfgBufLen < max) { cfgBuf[cfgBufLen++] = c; cfgBuf[cfgBufLen] = 0; }
    }
    needDraw = true;
}

static void onEnter() {
    if (screen == SCR_CHAT) {
        if (!inputLen && !radioUp) { // Enter sin texto y sin radio: reintentar
            startRadio();
            if (radioUp) { applyRouter(); showToast("Radio LoRa lista"); }
            else showToast("Sigue sin responder");
        } else if (inputLen) {
            sendText(input);
            inputLen = 0;
            input[0] = 0;
            chatScroll = 0;
        }
    } else if (screen == SCR_NODES) {
        int n = sortedNodes();
        if (n) openConvWith(R.nodes[nodeOrder[nodeSel]].num);
        nodeDetail = false;
    } else {
        if (cfgEditing) {
            cfgEditing = false;
            cfgCommitText();
        } else if (cfgIsText(cfgSel)) {
            cfgEditing = true;
            if (cfgSel == CF_PSK) {
                if (S.psk_len > 1) b64_encode(S.psk, S.psk_len, cfgBuf, sizeof(cfgBuf));
                else cfgBuf[0] = 0;
            } else {
                cfgValue(cfgSel, cfgBuf, sizeof(cfgBuf));
                if (cfgSel == CF_CHANNEL && !S.channel[0]) cfgBuf[0] = 0;
            }
            cfgBufLen = (int)strlen(cfgBuf);
        } else if (cfgSel >= CF_SEND_INFO) {
            cfgAction();
        } else {
            cfgChange(1);
        }
    }
    needDraw = true;
}

static void onDelete() {
    if (screen == SCR_CHAT && inputLen) {
        do inputLen--; while (inputLen && (input[inputLen] & 0xC0) == 0x80);
        input[inputLen] = 0;
    } else if (screen == SCR_CONFIG && cfgEditing && cfgBufLen) {
        cfgBuf[--cfgBufLen] = 0;
    } else if (screen == SCR_NODES) {
        nodeDetail = false;
    }
    needDraw = true;
}

static void pollKeys() {
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isChange()) return;
    auto k = M5Cardputer.Keyboard.keysState();
    char now[CPOS_KBD_MAX_KEYS + 1] = {0};
    int nn = 0;
    for (char c : k.word) if (nn < CPOS_KBD_MAX_KEYS) now[nn++] = c;
    for (int i = 0; i < nn; i++)
        if (!strchr(prevKeys, now[i])) onKey(now[i], k.fn);
    memcpy(prevKeys, now, sizeof(prevKeys));
    if (k.space && !prevSpace) {
        if (screen == SCR_NODES) { nodeDetail = !nodeDetail && R.num_nodes; needDraw = true; }
        else onKey(' ', false);
    }
    if (k.enter && !prevEnter) onEnter();
    if (k.del && !prevDel) onDelete();
    if (k.tab && !prevTab) {
        if (screen == SCR_CONFIG && cfgEditing) cfgEditing = false;
        screen = screen == SCR_CHAT ? SCR_NODES : screen == SCR_NODES ? SCR_CONFIG : SCR_CHAT;
        nodeDetail = false;
        if (screen == SCR_CHAT) clearUnread(conv);
        needDraw = true;
    }
    prevSpace = k.space;
    prevEnter = k.enter;
    prevDel = k.del;
    prevTab = k.tab;
}

// ---- arranque y bucle -------------------------------------------------------------------------------------------
static bool allocState() {
    if (!g_router) g_router = new Router();
    if (!msgs) msgs = (Msg *)calloc(MAX_MSGS, sizeof(Msg));
    return g_router && msgs;
}

static void startAll() {
    loadSettings();
    Platform pf{p_now, p_rnd, p_aes, p_send, p_busy, p_uplink};
    R.begin(pf, &app);
    applyRouter();
    loadStore();
    startRadio();
    setGps(S.gps);
}

static void pollRadio() {
    if (!radioUp) return;
    static cpos_lora_pkt_t pkt;
    while (cpos_api->lora.recv(&pkt)) R.on_receive(pkt.data, pkt.len, pkt.rssi, pkt.snr4 / 4.0f);
    R.loop();
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    spr.createSprite(240, 135);
    if (!allocState()) {
        spr.fillSprite(C_BG);
        spr.setTextColor(C_RED);
        spr.drawCenterString("Sin memoria", 120, 60);
        spr.pushSprite(0, 0);
        for (;;) delay(1000);
    }
    startAll();
    startMqtt();
    if (!radioUp) showToast(radioErr == -2 ? "No se encontró el Cap LoRa-1262" : "Radio LoRa no disponible");
}

void loop() {
    pollKeys();
    pollRadio();
    pollGnss();
    periodic();
    saveStore();
    static uint32_t lastDraw = 0;
    if (needDraw || millis() - lastDraw > 1000) {
        spr.fillSprite(C_BG);
        if (screen == SCR_CHAT) drawChat();
        else if (screen == SCR_NODES) drawNodes();
        else drawConfig();
        if (millis() < toastUntil) {
            spr.setFont(FONT);
            int w = spr.textWidth(toast) + 12;
            spr.fillRoundRect(120 - w / 2, 56, w, 18, 4, C_ACCENT);
            spr.setTextColor(C_BG);
            spr.drawCenterString(toast, 120, 59);
        }
        spr.pushSprite(0, 0);
        needDraw = false;
        lastDraw = millis();
    }
    delay(5);
}

// ---- intents (docs/intents.md) -------------------------------------------------------------------------------------
// espera a que la radio termine de mandar lo que hay en cola
static bool flush(uint32_t ms) {
    for (uint32_t t0 = millis(); millis() - t0 < ms;) {
        pollRadio();
        if (!R.queued() && !cpos_api->lora.pending()) return true;
        delay(10);
    }
    return false;
}

void on_intent(const Intent &in, IntentResult &out) {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, false);
    if (!allocState()) { out.error(CPOS_INTENT_FAILED); return; }
    loadSettings();
    Platform pf{p_now, p_rnd, p_aes, p_send, p_busy};
    R.begin(pf, &app);
    applyRouter();
    loadStore();

    if (in.is("mesh.nodes")) {
        int n = sortedNodes();
        for (int i = 0; i < n && i < 12; i++) {
            char tmp[24];
            const Node &nd = R.nodes[nodeOrder[i]];
            out.item(nd.has_user ? nd.user.short_name : nodeShort(nd.num, tmp, sizeof(tmp)));
        }
        out.done();
        return;
    }

    startRadio();
    if (!radioUp) { out.error(CPOS_INTENT_FAILED); return; }

    if (in.is("mesh.send")) {
        char text[100], to[24];
        if (!in.get("text", text, sizeof(text)) || !text[0]) { out.error(CPOS_INTENT_BAD_ARG); return; }
        conv = BROADCAST;
        if (in.get("to", to, sizeof(to)) && to[0]) {
            uint32_t dest = 0;
            if (to[0] == '!') dest = strtoul(to + 1, nullptr, 16);
            for (int i = 0; i < R.num_nodes && !dest; i++)
                if (R.nodes[i].has_user && !strcasecmp(R.nodes[i].user.short_name, to)) dest = R.nodes[i].num;
            if (!dest) { out.error(CPOS_INTENT_BAD_ARG); return; }
            conv = dest;
        }
        sendText(text);
        bool ok = flush(20000);
        saveStore(true);
        if (ok) out.ok(); else out.error(CPOS_INTENT_TIMEOUT);
    } else if (in.is("mesh.listen")) {
        int secs = in.i("seconds", 30);
        if (secs < 5 || secs > 120) { out.error(CPOS_INTENT_BAD_ARG); return; }
        int before = msgCount;
        for (uint32_t t0 = millis(); millis() - t0 < (uint32_t)secs * 1000;) {
            pollRadio();
            delay(10);
        }
        saveStore(true);
        int got = 0;
        for (int i = before; i < msgCount && got < 5; i++, got++) {
            char line[40], tmp[24];
            snprintf(line, sizeof(line), "%s: %.28s", nodeShort(msgs[i].from, tmp, sizeof(tmp)), msgs[i].text);
            for (char *p = line; *p; p++) if (*p == '|') *p = '/';
            out.item(line);
        }
        out.done();
    } else {
        out.error(CPOS_INTENT_NO_APP);
    }
}
