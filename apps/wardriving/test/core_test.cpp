// Test de host del núcleo de wardriving (sin OS ni radios: no se simula ningún dato de radio real; se
// arman advertisings y paquetes de prueba a mano solo para ejercitar parser, tablas y formateadores).
//
//   c++ -std=c++17 -Wall -Iapps/wardriving/src -Iapps/wardriving/test/stub -Iapps/ble-sentinel/src \
//       apps/wardriving/test/core_test.cpp apps/ble-sentinel/src/fingerprint.cpp -o /tmp/wd_test && \
//       /tmp/wd_test apps/wardriving/src/main.cpp
#include "wd_core.h"
#include "fingerprint.h" // el de BLE Sentinel, para comprobar que la identidad es la misma
#include <stdio.h>
#include <stdlib.h>
#include <string>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static WdGps fix(int32_t lat, int32_t lon) {
    WdGps g; g.valid = true; g.lat_e7 = lat; g.lon_e7 = lon; g.alt_m = 25; g.has_alt = true; g.acc_dm = 25; g.sats = 9; g.fix_age_ms = 400;
    return g;
}

// round trip CSV: formatea, reensambla (como al leer el .part con fgets) y vuelve a parsear
static bool roundtrip(const WdSample &in, WdSample &out) {
    char line[WD_LINE_MAX];
    int n = wd_format_csv(in, line, sizeof(line));
    if (n < 0) return false;
    WdRecAsm ra;
    // pedazos chicos para forzar el reensamblado
    std::string s(line);
    int got = 0;
    for (size_t k = 0; k < s.size(); k += 7) { std::string c = s.substr(k, 7); if (ra.feed(c.c_str())) got++; }
    if (got != 1) return false;
    char *f[WD_NFIELDS + 1];
    int nf = wd_csv_split(ra.buf, f, WD_NFIELDS + 1);
    return wd_sample_from_fields(f, nf, out);
}

static void adv(uint8_t *d, uint8_t &len, uint16_t company, uint8_t type, int8_t tx, const char *name, uint16_t uuid, bool rid, const uint8_t *mfg_extra, int extra) {
    len = 0;
    d[len++] = 2; d[len++] = 0x01; d[len++] = 0x06; // flags
    if (company != 0xFFFF) {
        d[len++] = (uint8_t)(3 + 1 + extra); d[len++] = 0xFF; d[len++] = company & 0xFF; d[len++] = company >> 8; d[len++] = type;
        for (int k = 0; k < extra; k++) d[len++] = mfg_extra[k];
    }
    if (tx != WD_NONE8) { d[len++] = 2; d[len++] = 0x0A; d[len++] = (uint8_t)tx; }
    if (uuid) { d[len++] = 3; d[len++] = 0x03; d[len++] = uuid & 0xFF; d[len++] = uuid >> 8; }
    if (rid) { d[len++] = 4; d[len++] = 0x16; d[len++] = 0xFA; d[len++] = 0xFF; d[len++] = 0x0D; }
    if (name) { uint8_t l = (uint8_t)strlen(name); if (len + 2 + l > 31) l = (uint8_t)(31 - len - 2); d[len++] = (uint8_t)(l + 1); d[len++] = 0x09; memcpy(d + len, name, l); len += l; }
}

static void test_csv_escaping() {
    WdSample m;
    m.ts_ms = 1234; m.source = WD_SRC_BLE;
    strcpy(m.identity, "ble:0123456789abcdef"); strcpy(m.addr, "AA:BB:CC:DD:EE:FF");
    m.rssi = -67;
    strcpy(m.name, "Juan, \"el\" Tag\nB");
    strcpy(m.category, "mfg:apple");
    m.gps = fix(-329476000, -606307000);
    char line[WD_LINE_MAX];
    int n = wd_format_csv(m, line, sizeof(line));
    CHECK(n > 0 && line[n - 1] == '\n');
    CHECK(strstr(line, ",\"Juan, \"\"el\"\" Tag\nB\",") != nullptr); // coma, comillas y newline entre comillas
    WdSample r;
    CHECK(roundtrip(m, r));
    CHECK(!strcmp(r.name, m.name));
    CHECK(r.rssi == -67 && r.ts_ms == 1234 && r.source == WD_SRC_BLE);

    // fórmula de planilla: lleva ' delante y vuelve a su valor al leer
    strcpy(m.name, "=HYPERLINK(\"x\")");
    n = wd_format_csv(m, line, sizeof(line));
    CHECK(strstr(line, ",\"'=HYPERLINK(\"\"x\"\")\",") != nullptr);
    CHECK(roundtrip(m, r) && !strcmp(r.name, m.name));
    // espacios en los bordes: entre comillas
    strcpy(m.name, " pad ");
    n = wd_format_csv(m, line, sizeof(line));
    CHECK(strstr(line, ",\" pad \",") != nullptr);
}

static void test_empty_fields() {
    WdSample m; // sin RSSI, SNR, frecuencia, canal, nombre, GPS, payload
    m.ts_ms = 5; m.source = WD_SRC_LORA; strcpy(m.identity, "lora:00");
    char line[WD_LINE_MAX];
    int n = wd_format_csv(m, line, sizeof(line));
    CHECK(n > 0);
    CHECK(!strcmp(line, "5,lora,lora:00,,,,,,,,,,,,0,false,\n"));
    char copy[WD_LINE_MAX]; strcpy(copy, line);
    char *f[WD_NFIELDS + 1];
    CHECK(wd_csv_split(copy, f, WD_NFIELDS + 1) == WD_NFIELDS);
    // encabezado estable: 17 columnas en el orden del brief
    char h[] = WD_CSV_HEADER;
    CHECK(wd_csv_split(h, f, WD_NFIELDS + 1) == WD_NFIELDS);
    CHECK(!strcmp(f[0], "timestamp_ms") && !strcmp(f[10], "lat") && !strcmp(f[11], "lon") && !strcmp(f[16], "payload_hex"));
    WdSample r;
    CHECK(roundtrip(m, r) && !r.gps.valid && r.rssi == WD_NONE16 && r.snr4 == WD_NONE8 && !r.freq_hz && !*r.name);
    // sin fix: el GeoJSON no inventa un punto
    n = wd_format_feature(m, line, sizeof(line));
    CHECK(n > 0 && strstr(line, "\"geometry\":null") && strstr(line, "\"gps_valid\":false") && strstr(line, "\"rssi_dbm\":null"));
}

static void test_coords() {
    WdSample m; m.source = WD_SRC_BLE; strcpy(m.identity, "ble:1");
    m.gps = fix(-329476000, -606307000);
    char line[WD_LINE_MAX], feat[WD_FEAT_MAX];
    wd_format_csv(m, line, sizeof(line));
    CHECK(strstr(line, ",-32.9476000,-60.6307000,25,2.5,9,true,") != nullptr);
    int n = wd_format_feature(m, feat, sizeof(feat));
    CHECK(n > 0);
    // RFC 7946: [lon, lat(, alt)]
    CHECK(strstr(feat, "\"coordinates\":[-60.6307000,-32.9476000,25]") != nullptr);
    CHECK(!strncmp(feat, "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\"", 44));
    // bordes y valores chicos negativos (el signo no se pierde con parte entera 0)
    m.gps = fix(-1, 1800000000);
    wd_format_csv(m, line, sizeof(line));
    CHECK(strstr(line, ",-0.0000001,180.0000000,") != nullptr);
    WdSample r;
    CHECK(roundtrip(m, r) && r.gps.lat_e7 == -1 && r.gps.lon_e7 == 1800000000);
    m.gps = fix(900000000, -1799999999);
    CHECK(roundtrip(m, r) && r.gps.lat_e7 == 900000000 && r.gps.lon_e7 == -1799999999);
    // fuera de WGS84: la fila se rechaza al recuperar
    char bad[] = "1,ble,ble:1,,,,,,,,91.0000000,0.0000000,,,3,true,\n";
    char *f[WD_NFIELDS + 1];
    int nf = wd_csv_split(bad, f, WD_NFIELDS + 1);
    CHECK(!wd_sample_from_fields(f, nf, r));
    // coordenadas sin fix: tampoco es una fila válida
    char nofix[] = "1,ble,ble:1,,,,,,,,1.0000000,1.0000000,,,3,false,\n";
    nf = wd_csv_split(nofix, f, WD_NFIELDS + 1);
    CHECK(!wd_sample_from_fields(f, nf, r));
}

static void test_ble_identity() {
    uint8_t d[31], len;
    const uint8_t extra[] = {0x19, 0x00, 0xAA, 0xBB};
    adv(d, len, 0x004C, 0x12, -8, nullptr, 0, false, extra, 4);
    WdBleObs o; wd_parse_adv(d, len, o);
    CHECK(o.company == 0x004C && o.mfg_type == 0x12 && o.tx == -8 && o.payload_len == len);

    // misma cuenta que BLE Sentinel
    fingerprint::AdvInput fin;
    fin.companyId = o.company; fin.mfgTypeByte = o.mfg_type; fin.payloadLen = o.payload_len; fin.txPowerDbm = o.tx;
    CHECK(fingerprint::compute(fin).hash == wd_ble_hash(o));
    uint8_t d2[31], len2;
    adv(d2, len2, 0xFFFF, 0, WD_NONE8, "Band", 0x180D, true, nullptr, 0);
    WdBleObs o2; wd_parse_adv(d2, len2, o2);
    fingerprint::AdvInput fin2;
    fin2.payloadLen = o2.payload_len; fin2.serviceUuids16 = {0x180D}; fin2.remoteIdService = true;
    CHECK(o2.rid && o2.nuuids == 1 && !strcmp(o2.name, "Band"));
    CHECK(fingerprint::compute(fin2).hash == wd_ble_hash(o2));

    // rotación de MAC: misma identidad, una sola entrada, la MAC nueva queda como dato efímero
    WdStore *s = new WdStore();
    uint8_t mac1[6] = {1, 2, 3, 4, 5, 0xC1}, mac2[6] = {9, 8, 7, 6, 5, 0xD4};
    WdGps g = fix(-329476000, -606307000);
    int a = s->ble_observe(o, mac1, -60, 1000, g, true);
    int b = s->ble_observe(o, mac2, -70, 1000 + 15 * 60 * 1000, g, true);
    CHECK(a == 0 && b == 0 && s->nble == 1);
    CHECK(s->ble[0].rmin == -70 && s->ble[0].rmax == -60 && s->ble[0].n == 2);
    CHECK(fingerprint::isPlausibleMacRotation(15 * 60 * 1000));
    WdSample m;
    s->sample(0, m);
    CHECK(!strncmp(m.identity, "ble:", 4) && strncmp(m.identity, "ble:w:", 6) && strlen(m.identity) == 20);
    CHECK(!strcmp(m.addr, "C1:05:04:03:02:01"));
    CHECK(!strcmp(m.category, "findmy"));
    CHECK(s->sweep(2000, g, true) && s->ring_n == 2); // la ventana con la 2.ª MAC sale en el barrido
    s->sample(1, m);
    CHECK(!strcmp(m.addr, "D4:05:06:07:08:09") && m.rssi == -70);
    char id0[24]; wd_ble_ident_str(s->ble[0], id0);
    CHECK(!strcmp(m.identity, id0)); // la identidad no cambió con la MAC
    char js[WD_FEAT_MAX];
    CHECK(wd_format_ble_summary(s->ble[0], js, sizeof(js)) > 0);
    CHECK(strstr(js, "\"rssi_min\":-70,\"rssi_max\":-60,\"rssi_avg\":-65.0,\"adverts\":2") != nullptr);

    // payload pobre (solo flags): identidad débil marcada, depende también de la MAC
    uint8_t d3[31] = {2, 0x01, 0x06}; WdBleObs o3; wd_parse_adv(d3, 3, o3);
    s->reset();
    s->ble_observe(o3, mac1, -50, 1, g, true);
    s->ble_observe(o3, mac2, -50, 2, g, true);
    CHECK(s->nble == 2);
    s->sample(0, m);
    CHECK(!strncmp(m.identity, "ble:w:", 6) && !strcmp(m.category, "unknown-weak"));
    delete s;
}

static void test_limits() {
    WdStore *s = new WdStore();
    WdGps g; // sin fix
    uint8_t mac[6] = {0};
    // 48 identidades BLE, el resto se descarta con contador
    for (int k = 0; k < 60; k++) {
        uint8_t d[31], len; adv(d, len, (uint16_t)(0x0100 + k), 1, WD_NONE8, nullptr, 0, false, nullptr, 0);
        WdBleObs o; wd_parse_adv(d, len, o);
        if (s->ring_full()) s->ring_n = 0; // "vaciado" del lote
        s->ble_observe(o, mac, -80, (uint32_t)k, g, true);
    }
    CHECK(s->nble == WD_MAX_BLE && s->drop_ble == 12);
    // ring de 32: sin vaciar, lo que no entra se cuenta
    s->reset();
    for (int k = 0; k < 40; k++) {
        uint8_t d[31], len; adv(d, len, (uint16_t)(0x0200 + k), 1, WD_NONE8, nullptr, 0, false, nullptr, 0);
        WdBleObs o; wd_parse_adv(d, len, o);
        s->ble_observe(o, mac, -80, (uint32_t)k, g, true);
    }
    CHECK(s->ring_n == WD_RING && s->drop_ring == 8 && s->ring_full());
    // el barrido se pausa con el ring lleno y sigue después de vaciarlo
    for (int k = 0; k < 40; k++) s->ble[k].an = 1;
    CHECK(!s->sweep(10, g, true) && s->ring_n == WD_RING);
    s->ring_n = 0;
    CHECK(!s->sweep(10, g, true) && s->ring_n == WD_RING); // 40 filas: 32 ...
    s->ring_n = 0;
    CHECK(s->sweep(10, g, true) && s->ring_n == 8);         // ... y las 8 restantes
    CHECK(s->sweep(11, g, true) && s->ring_n == 8);         // nada nuevo: no repite filas
    // 32 identidades LoRa; payload hex <= 48
    s->reset();
    uint8_t pkt[255];
    for (int k = 0; k < 255; k++) pkt[k] = (uint8_t)k;
    for (int k = 0; k < 40; k++) { pkt[0] = (uint8_t)k; if (s->ring_full()) s->ring_n = 0; s->lora_observe(915000000, -110, -30, pkt, 255, 1, g, true); }
    CHECK(s->nlora == WD_MAX_LORA && s->drop_lora == 8);
    s->ring_n = 0;
    pkt[0] = 3;
    CHECK(s->lora_observe(915000000, -110, -30, pkt, 255, 2, g, true) == 3); // identidad ya conocida
    CHECK(s->sweep(3, g, true) && s->ring_n == 1);
    WdSample m; s->sample(0, m);
    CHECK(strlen(m.payload) == WD_PAYLOAD_HEX && m.freq_hz == 915000000 && m.snr4 == -30 && m.rssi == -110);
    char line[WD_LINE_MAX];
    CHECK(wd_format_csv(m, line, sizeof(line)) > 0 && strstr(line, ",-7.50,915000000,"));
    // nombre: <= 24 bytes UTF-8 sin cortar caracteres, inválidos -> '?', control -> ' '
    char out[WD_NAME_MAX + 1];
    const char *ene = "ñññññññññññññññ"; // 30 bytes
    wd_sanitize_name((const uint8_t *)ene, strlen(ene), out);
    CHECK(strlen(out) == 24 && !strncmp(out, ene, 24));
    const uint8_t bad[] = {'a', 0xFF, 'b', '\n', 0xC3}; // 0xC3 cortado al final
    wd_sanitize_name(bad, sizeof(bad), out);
    CHECK(!strcmp(out, "a?b ?"));
    // peor caso de línea (todos los campos al máximo, nombre lleno de comillas) entra en 320
    WdSample w; w.ts_ms = 0xFFFFFFFF; w.source = WD_SRC_LORA;
    memset(w.identity, 'x', 23); memset(w.addr, 'y', 17);
    w.rssi = -32767; w.snr4 = -128; w.freq_hz = 0xFFFFFFFF; w.channel = 65535;
    memset(w.name, '"', WD_NAME_MAX); memset(w.category, 'c', 15); memset(w.payload, 'f', WD_PAYLOAD_HEX);
    w.gps = fix(-900000000, -1800000000); w.gps.alt_m = -2147483647 - 1; w.gps.acc_dm = 65534; w.gps.sats = 255;
    int n = wd_format_csv(w, line, sizeof(line));
    CHECK(n > 0 && n < WD_LINE_MAX);
    CHECK(wd_format_csv(w, line, 64) == -1); // no entra: se informa, no se corta en silencio
    char feat[WD_FEAT_MAX];
    CHECK(wd_format_feature(w, feat, sizeof(feat)) > 0);
    delete s;
}

// .part cortado por un corte de energía: filas completas se recuperan, la última a medias se descarta
static void test_part_recovery() {
    std::string part = WD_CSV_HEADER "\n";
    WdSample m; m.source = WD_SRC_BLE; strcpy(m.identity, "ble:aa"); m.gps = fix(-345, 678);
    char line[WD_LINE_MAX];
    for (int k = 0; k < 5; k++) {
        m.ts_ms = (uint32_t)k;
        strcpy(m.name, k == 2 ? "con\nsalto, y \"comillas\"" : "x");
        wd_format_csv(m, line, sizeof(line));
        part += line;
    }
    part += "5,ble,ble:aa,,-6"; // escritura interrumpida
    FILE *f = tmpfile();
    fwrite(part.data(), 1, part.size(), f);
    rewind(f);
    WdRecAsm ra;
    int rows = 0, bad = 0, header = 0;
    std::string geo = WD_GEOJSON_HEAD;
    char buf[WD_LINE_MAX], feat[WD_FEAT_MAX];
    while (fgets(buf, sizeof(buf), f)) {
        if (!ra.feed(buf)) continue;
        if (!strcmp(ra.buf, WD_CSV_HEADER "\n")) { header++; continue; }
        char *fl[WD_NFIELDS + 1];
        int nf = wd_csv_split(ra.buf, fl, WD_NFIELDS + 1);
        WdSample r;
        if (!wd_sample_from_fields(fl, nf, r)) { bad++; continue; }
        if (rows == 2) CHECK(!strcmp(r.name, "con\nsalto, y \"comillas\""));
        CHECK(wd_format_feature(r, feat, sizeof(feat)) > 0);
        if (rows) geo += ",\n";
        geo += feat;
        rows++;
    }
    geo += WD_GEOJSON_TAIL;
    fclose(f);
    CHECK(header == 1 && rows == 5 && bad == 0);
    CHECK(ra.leftover()); // la fila cortada se detecta y no se cuenta
    CHECK(geo.find("\"name\":\"con\\nsalto, y \\\"comillas\\\"\"") != std::string::npos);
    CHECK(geo.rfind("\n]}\n") == geo.size() - 4);
    // basura en el medio: se cuenta como inválida y se sigue
    char junk[] = "hola,mundo\n";
    char *fl[WD_NFIELDS + 1];
    WdSample r;
    CHECK(!wd_sample_from_fields(fl, wd_csv_split(junk, fl, WD_NFIELDS + 1), r));
    char unterminated[] = "1,ble,\"abc\n";
    CHECK(wd_csv_split(unterminated, fl, WD_NFIELDS + 1) < 0);
    // registro más largo que una línea: se saltea entero
    WdRecAsm rb;
    std::string longrec(WD_LINE_MAX - 1, 'z');
    CHECK(!rb.feed(longrec.c_str()) && !rb.feed("zzz\n") && rb.too_long == 1);
    CHECK(rb.feed("1,ble\n") == 1 && !strcmp(rb.buf, "1,ble\n"));
}

// La app solo escucha: ninguna llamada que transmita, conecte, asocie o anuncie en main.cpp
static void test_passive_source(const char *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::string src, code;
    char b[512];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) src.append(b, n);
    fclose(f);
    // sin comentarios ni strings, para mirar solo el código
    for (size_t k = 0; k < src.size(); k++) {
        if (src.compare(k, 2, "//") == 0) { while (k < src.size() && src[k] != '\n') k++; }
        else if (src.compare(k, 2, "/*") == 0) { k = src.find("*/", k + 2); if (k == std::string::npos) break; k++; }
        else if (src[k] == '"') { k++; while (k < src.size() && src[k] != '"') { if (src[k] == '\\') k++; k++; } code += "\"\""; }
        else code += src[k];
    }
    const char *forbidden[] = {"lora.send", "lora.configure", "gnss_write", "ble.connect", "ble.write", "ble.read", "ble.subscribe",
                               "ble.services", "ble.characteristics", "ble.set_power", "ble_srv", "net.connect", "net.scan_start",
                               "net.socket", "net.sendto", "net.sock_connect", "net.sock_send", "net.resolve", "net.mode(1"};
    for (const char *t : forbidden) {
        bool found = code.find(t) != std::string::npos;
        if (found) printf("  llamada prohibida en %s: %s\n", path, t);
        CHECK(!found);
    }
    CHECK(code.find("ble.scan_start(0, 0,") != std::string::npos); // active = 0: scan pasivo
    CHECK(code.find("net.mode(0)") != std::string::npos);          // WiFi del OS apagado mientras corre
}

int main(int argc, char **argv) {
    test_csv_escaping();
    test_empty_fields();
    test_coords();
    test_ble_identity();
    test_limits();
    test_part_recovery();
    test_passive_source(argc > 1 ? argv[1] : "apps/wardriving/src/main.cpp");
    printf("%d checks, %d fails\n", checks, fails);
    return fails ? 1 : 0;
}
