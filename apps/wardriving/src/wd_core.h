// Núcleo de wardriving: identidad BLE, tablas acotadas, ring de muestras y formateadores CSV/GeoJSON.
//
// No depende del OS (ni de cpos/abi.h ni de libc más allá de string.h/stdint.h): lo compilan tanto la app
// como el test de host (apps/wardriving/test/core_test.cpp). Todo es de tamaño fijo, sin heap, sin STL.
//
// Todo lo de acá es escucha pasiva: procesa lo que las radios ya recibieron. No hay nada que transmita,
// conecte, asocie ni autentique; tampoco interpreta payloads LoRa (solo los muestra en hex truncado).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ---- límites de v1 (BRIEF-wardriving-v1) --------------------------------------------------
#define WD_MAX_BLE 48
#define WD_MAX_WIFI 32 // reservado: el backend WiFi es `unsupported` con la ABI actual, no hay tabla
#define WD_MAX_LORA 32
#define WD_RING 32
#define WD_PAYLOAD_HEX 48 // caracteres hex (24 bytes)
#define WD_NAME_MAX 24    // bytes UTF-8
#define WD_LINE_MAX 320   // línea CSV, con el \n
#define WD_FEAT_MAX 900   // un Feature GeoJSON
#define WD_NFIELDS 17
#define WD_UUIDS 8
#define WD_FIX_MAX_AGE_MS 3000 // un fix más viejo que esto se registra con gps_valid=false

#define WD_CSV_HEADER \
    "timestamp_ms,source,identity,mac_or_addr,rssi_dbm,snr_db,frequency_hz,channel,name,category," \
    "lat,lon,alt_m,accuracy_m,satellites,gps_valid,payload_hex"

enum { WD_SRC_BLE = 0, WD_SRC_WIFI = 1, WD_SRC_LORA = 2 };
static const char *const WD_SRC_NAMES[3] = {"ble", "wifi", "lora"};

#define WD_NONE16 ((int16_t)-32768)
#define WD_NONE8 ((int8_t)-128)

struct WdGps {
    int32_t lat_e7 = 0, lon_e7 = 0; // WGS84, grados * 1e7
    int32_t alt_m = 0;
    uint32_t fix_age_ms = 0;        // edad del fix al tomar la muestra (en RAM; no es columna del CSV)
    uint16_t acc_dm = 0xFFFF;       // precisión estimada en decímetros, 0xFFFF = sin dato
    uint8_t sats = 0;
    bool valid = false;
    bool has_alt = false;
};

struct WdSample {
    uint32_t ts_ms = 0;
    uint8_t source = WD_SRC_BLE;
    char identity[24] = "";
    char addr[18] = "";
    int16_t rssi = WD_NONE16;
    int8_t snr4 = WD_NONE8;          // cuartos de dB
    uint32_t freq_hz = 0;            // 0 = vacío
    uint16_t channel = 0;            // 0 = vacío
    char name[WD_NAME_MAX + 1] = "";
    char category[16] = "";
    WdGps gps;
    char payload[WD_PAYLOAD_HEX + 1] = "";
};

// ---- salida acotada -------------------------------------------------------------------------
struct WdOut {
    char *b;
    size_t cap, n = 0;
    bool ovf = false;
    WdOut(char *buf, size_t c) : b(buf), cap(c) { if (c) b[0] = 0; }
    void c(char ch) { if (n + 1 < cap) { b[n++] = ch; b[n] = 0; } else ovf = true; }
    void s(const char *p) { while (*p) c(*p++); }
    void u(uint64_t v) { char t[21]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) c(t[--k]); }
    void i(int64_t v) { if (v < 0) { c('-'); u((uint64_t)(-v)); } else u((uint64_t)v); }
    void frac(int64_t v, uint32_t scale, int digits) { // v / scale con `digits` decimales fijos
        if (v < 0) { c('-'); v = -v; }
        u((uint64_t)v / scale);
        c('.');
        uint64_t f = (uint64_t)v % scale;
        char t[12];
        for (int k = digits - 1; k >= 0; k--) { t[k] = (char)('0' + f % 10); f /= 10; }
        for (int k = 0; k < digits; k++) c(t[k]);
    }
    void hex(uint64_t v, int digits) { for (int k = digits - 1; k >= 0; k--) c("0123456789abcdef"[(v >> (k * 4)) & 15]); }
};

// ---- texto ------------------------------------------------------------------------------------
// Nombre BLE a UTF-8 válido de a lo sumo WD_NAME_MAX bytes: secuencias inválidas -> '?', control -> ' ',
// nunca corta un carácter a la mitad.
static void wd_sanitize_name(const uint8_t *in, size_t len, char *out) {
    size_t o = 0;
    for (size_t k = 0; k < len && in[k];) {
        uint8_t b = in[k];
        size_t sl = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = sl > 0 && k + sl <= len && !(sl == 2 && b < 0xC2);
        for (size_t j = 1; ok && j < sl; j++) ok = (in[k + j] & 0xC0) == 0x80;
        if (!ok) sl = 1;
        if (o + (ok ? sl : 1) > WD_NAME_MAX) break;
        if (!ok) out[o++] = '?';
        else if (sl == 1) out[o++] = (b < 0x20 || b == 0x7F) ? ' ' : (char)b;
        else for (size_t j = 0; j < sl; j++) out[o++] = (char)in[k + j];
        k += sl;
    }
    out[o] = 0;
}

static void wd_hex(const uint8_t *d, size_t n, char *out, size_t max_chars) {
    size_t o = 0;
    for (size_t k = 0; k < n && o + 2 <= max_chars; k++) {
        out[o++] = "0123456789abcdef"[d[k] >> 4];
        out[o++] = "0123456789abcdef"[d[k] & 15];
    }
    out[o] = 0;
}

static void wd_mac_str(const uint8_t a[6], char *out) { // little endian como lo da la ABI
    WdOut w(out, 18);
    for (int k = 5; k >= 0; k--) { w.hex(a[k], 2); if (k) w.c(':'); }
    for (char *p = out; *p; p++) if (*p >= 'a') *p = (char)(*p - 32);
}

// campo CSV (RFC 4180). Comillas si trae , " CR LF o espacios en los bordes. Un texto que una planilla
// tomaría por fórmula (= + - @ tab al principio) lleva un ' delante: los nombres BLE los elige cualquiera.
static void wd_csv_field(WdOut &w, const char *s, bool text) {
    bool formula = text && (*s == '=' || *s == '+' || *s == '-' || *s == '@' || *s == '\t');
    size_t l = strlen(s);
    bool q = formula || (l && (s[0] == ' ' || s[l - 1] == ' '));
    for (const char *p = s; *p && !q; p++) q = *p == ',' || *p == '"' || *p == '\r' || *p == '\n';
    if (!q) { w.s(s); return; }
    w.c('"');
    if (formula) w.c('\'');
    for (const char *p = s; *p; p++) { if (*p == '"') w.c('"'); w.c(*p); }
    w.c('"');
}

static void wd_json_str(WdOut &w, const char *s) {
    w.c('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { w.c('\\'); w.c((char)*p); }
        else if (*p == '\n') w.s("\\n");
        else if (*p == '\r') w.s("\\r");
        else if (*p == '\t') w.s("\\t");
        else if (*p < 0x20) { w.s("\\u00"); w.hex(*p, 2); }
        else w.c((char)*p);
    }
    w.c('"');
}

// ---- CSV ----------------------------------------------------------------------------------------
// una fila con \n final; devuelve el largo o -1 si no entra en `cap` (no escribe nada útil en ese caso)
static int wd_format_csv(const WdSample &m, char *out, size_t cap) {
    WdOut w(out, cap);
    w.u(m.ts_ms); w.c(',');
    w.s(WD_SRC_NAMES[m.source < 3 ? m.source : 0]); w.c(',');
    wd_csv_field(w, m.identity, false); w.c(',');
    wd_csv_field(w, m.addr, false); w.c(',');
    if (m.rssi != WD_NONE16) w.i(m.rssi);
    w.c(',');
    if (m.snr4 != WD_NONE8) w.frac(m.snr4 * 25, 100, 2);
    w.c(',');
    if (m.freq_hz) w.u(m.freq_hz);
    w.c(',');
    if (m.channel) w.u(m.channel);
    w.c(',');
    wd_csv_field(w, m.name, true); w.c(',');
    wd_csv_field(w, m.category, false); w.c(',');
    if (m.gps.valid) { w.frac(m.gps.lat_e7, 10000000, 7); w.c(','); w.frac(m.gps.lon_e7, 10000000, 7); w.c(','); }
    else w.s(",,");
    if (m.gps.valid && m.gps.has_alt) w.i(m.gps.alt_m);
    w.c(',');
    if (m.gps.valid && m.gps.acc_dm != 0xFFFF) w.frac(m.gps.acc_dm, 10, 1);
    w.c(',');
    w.u(m.gps.sats); w.c(',');
    w.s(m.gps.valid ? "true" : "false"); w.c(',');
    w.s(m.payload);
    w.c('\n');
    return w.ovf ? -1 : (int)w.n;
}

// Divide un registro CSV (sin o con \r\n final) en campos, en el lugar, desescapando comillas.
static int wd_csv_split(char *rec, char **f, int max) {
    size_t l = strlen(rec);
    while (l && (rec[l - 1] == '\n' || rec[l - 1] == '\r')) rec[--l] = 0;
    int nf = 0;
    char *r = rec, *wp = rec;
    for (;;) {
        if (nf == max) return -1;
        f[nf++] = wp;
        if (*r == '"') {
            r++;
            for (;;) {
                if (!*r) return -1; // comilla sin cerrar
                if (*r == '"') { if (r[1] == '"') { *wp++ = '"'; r += 2; continue; } r++; break; }
                *wp++ = *r++;
            }
            if (*r && *r != ',') return -1;
        } else {
            while (*r && *r != ',') *wp++ = *r++;
        }
        bool more = *r == ',';
        if (*r) r++;
        *wp++ = 0;
        if (!more) break;
    }
    return nf;
}

static bool wd_parse_int(const char *s, int64_t lo, int64_t hi, int64_t &out) {
    bool neg = *s == '-';
    if (neg) s++;
    if (!*s) return false;
    int64_t v = 0;
    for (; *s; s++) { if (*s < '0' || *s > '9' || v > 100000000000LL) return false; v = v * 10 + (*s - '0'); }
    out = neg ? -v : v;
    return out >= lo && out <= hi;
}

// decimal con `digits` decimales a entero escalado ("-32.9476" con 7 -> -329476000); trunca el resto
static bool wd_parse_fixed(const char *s, int digits, int64_t &out) {
    bool neg = *s == '-';
    if (neg) s++;
    if (!*s || *s == '.') return false;
    int64_t v = 0;
    for (; *s && *s != '.'; s++) { if (*s < '0' || *s > '9' || v > 100000) return false; v = v * 10 + (*s - '0'); }
    int d = 0;
    if (*s == '.') {
        s++;
        if (!*s) return false;
        for (; *s; s++) { if (*s < '0' || *s > '9') return false; if (d < digits) { v = v * 10 + (*s - '0'); d++; } }
    }
    for (; d < digits; d++) v *= 10;
    out = neg ? -v : v;
    return true;
}

static int32_t wd_rdiv(int32_t a, int32_t b) { return a >= 0 ? (a + b / 2) / b : (a - b / 2) / b; } // redondeo al más cercano

static void wd_copy(char *dst, const char *src, size_t cap) { size_t l = strlen(src); if (l >= cap) l = cap - 1; memcpy(dst, src, l); dst[l] = 0; }

// Campos de una fila (los de wd_csv_split) a muestra. false si la fila no es del formato de esta app.
static bool wd_sample_from_fields(char **f, int nf, WdSample &m) {
    if (nf != WD_NFIELDS) return false;
    m = WdSample();
    int64_t v;
    if (!wd_parse_int(f[0], 0, 0xFFFFFFFFLL, v)) return false;
    m.ts_ms = (uint32_t)v;
    m.source = 255;
    for (uint8_t k = 0; k < 3; k++) if (!strcmp(f[1], WD_SRC_NAMES[k])) m.source = k;
    if (m.source == 255 || !*f[2]) return false;
    wd_copy(m.identity, f[2], sizeof(m.identity));
    wd_copy(m.addr, f[3], sizeof(m.addr));
    if (*f[4]) { if (!wd_parse_int(f[4], -200, 50, v)) return false; m.rssi = (int16_t)v; }
    if (*f[5]) { if (!wd_parse_fixed(f[5], 2, v) || v % 25) return false; m.snr4 = (int8_t)(v / 25); }
    if (*f[6]) { if (!wd_parse_int(f[6], 1, 0xFFFFFFFFLL, v)) return false; m.freq_hz = (uint32_t)v; }
    if (*f[7]) { if (!wd_parse_int(f[7], 1, 65535, v)) return false; m.channel = (uint16_t)v; }
    const char *name = f[8];
    if (name[0] == '\'' && (name[1] == '=' || name[1] == '+' || name[1] == '-' || name[1] == '@' || name[1] == '\t')) name++;
    wd_copy(m.name, name, sizeof(m.name));
    wd_copy(m.category, f[9], sizeof(m.category));
    if (!strcmp(f[15], "true")) m.gps.valid = true;
    else if (strcmp(f[15], "false")) return false;
    if (m.gps.valid) {
        if (!wd_parse_fixed(f[10], 7, v) || v < -900000000LL || v > 900000000LL) return false;
        m.gps.lat_e7 = (int32_t)v;
        if (!wd_parse_fixed(f[11], 7, v) || v < -1800000000LL || v > 1800000000LL) return false;
        m.gps.lon_e7 = (int32_t)v;
        if (*f[12]) { if (!wd_parse_int(f[12], -1000, 100000, v)) return false; m.gps.alt_m = (int32_t)v; m.gps.has_alt = true; }
        if (*f[13]) { if (!wd_parse_fixed(f[13], 1, v) || v < 0 || v >= 0xFFFF) return false; m.gps.acc_dm = (uint16_t)v; }
    } else if (*f[10] || *f[11]) {
        return false; // coordenadas sin fix: no es una fila nuestra
    }
    if (!wd_parse_int(f[14], 0, 255, v)) return false;
    m.gps.sats = (uint8_t)v;
    if (strlen(f[16]) > WD_PAYLOAD_HEX) return false;
    wd_copy(m.payload, f[16], sizeof(m.payload));
    return true;
}

// ---- GeoJSON (RFC 7946) -------------------------------------------------------------------------
// Un Feature. Sin fix: geometry null (RFC 7946 §3.2). Coordenadas [lon, lat(, alt)].
static int wd_format_feature(const WdSample &m, char *out, size_t cap) {
    WdOut w(out, cap);
    w.s("{\"type\":\"Feature\",\"geometry\":");
    if (m.gps.valid) {
        w.s("{\"type\":\"Point\",\"coordinates\":[");
        w.frac(m.gps.lon_e7, 10000000, 7); w.c(',');
        w.frac(m.gps.lat_e7, 10000000, 7);
        if (m.gps.has_alt) { w.c(','); w.i(m.gps.alt_m); }
        w.s("]}");
    } else w.s("null");
    w.s(",\"properties\":{\"timestamp_ms\":"); w.u(m.ts_ms);
    w.s(",\"source\":"); wd_json_str(w, WD_SRC_NAMES[m.source < 3 ? m.source : 0]);
    w.s(",\"identity\":"); wd_json_str(w, m.identity);
    w.s(",\"mac_or_addr\":"); if (*m.addr) wd_json_str(w, m.addr); else w.s("null");
    w.s(",\"rssi_dbm\":"); if (m.rssi != WD_NONE16) w.i(m.rssi); else w.s("null");
    w.s(",\"snr_db\":"); if (m.snr4 != WD_NONE8) w.frac(m.snr4 * 25, 100, 2); else w.s("null");
    w.s(",\"frequency_hz\":"); if (m.freq_hz) w.u(m.freq_hz); else w.s("null");
    w.s(",\"channel\":"); if (m.channel) w.u(m.channel); else w.s("null");
    w.s(",\"name\":"); if (*m.name) wd_json_str(w, m.name); else w.s("null");
    w.s(",\"category\":"); if (*m.category) wd_json_str(w, m.category); else w.s("null");
    w.s(",\"alt_m\":"); if (m.gps.valid && m.gps.has_alt) w.i(m.gps.alt_m); else w.s("null");
    w.s(",\"accuracy_m\":"); if (m.gps.valid && m.gps.acc_dm != 0xFFFF) w.frac(m.gps.acc_dm, 10, 1); else w.s("null");
    w.s(",\"satellites\":"); w.u(m.gps.sats);
    w.s(",\"gps_valid\":"); w.s(m.gps.valid ? "true" : "false");
    w.s(",\"payload_hex\":"); if (*m.payload) wd_json_str(w, m.payload); else w.s("null");
    w.s("}}");
    return w.ovf ? -1 : (int)w.n;
}
#define WD_GEOJSON_HEAD "{\"type\":\"FeatureCollection\",\"features\":[\n"
#define WD_GEOJSON_TAIL "\n]}\n"

// ---- lectura de registros CSV por pedazos (recuperación del .part) -----------------------------
// Se alimenta con lo que da fgets (pedazos de hasta WD_LINE_MAX-1 bytes). Un registro termina en \n
// fuera de comillas. Lo que queda a medias al final del archivo (corte de energía) se descarta.
struct WdRecAsm {
    char buf[WD_LINE_MAX];
    size_t n = 0;
    bool skipping = false; // registro demasiado largo: se saltea hasta el próximo \n
    uint32_t too_long = 0;
    // 1 = buf tiene un registro completo; 0 = falta más
    int feed(const char *chunk) {
        size_t l = strlen(chunk);
        bool nl = l && chunk[l - 1] == '\n';
        if (skipping) { if (nl) skipping = false; return 0; }
        if (n + l >= sizeof(buf)) { too_long++; n = 0; skipping = !nl; return 0; }
        memcpy(buf + n, chunk, l + 1);
        n += l;
        if (!nl) return 0;
        size_t q = 0;
        for (size_t k = 0; k < n; k++) q += buf[k] == '"';
        if (q & 1) return 0; // \n dentro de comillas: el registro sigue
        n = 0;
        return 1;
    }
    bool leftover() const { return n > 0 || skipping; } // al EOF: había un registro cortado
};

// ---- identidad BLE (modelo BLE Sentinel) --------------------------------------------------------
// Mismo hash que apps/ble-sentinel/src/fingerprint.cpp (FNV-1a 64 sobre company ID, type byte del
// manufacturer data, largo del payload, TX power en buckets de 4 dB, máscara de AD types (0 en v1, igual
// que Sentinel), Remote ID y UUIDs de 16 bits ordenados). Se reescribe acá porque el original usa
// Arduino.h y std::vector; la MAC no entra al hash.
struct WdBleObs {
    uint16_t company = 0xFFFF;
    uint8_t mfg_type = 0;
    uint8_t payload_len = 0;
    int8_t tx = WD_NONE8;
    uint16_t uuids[WD_UUIDS];
    uint8_t nuuids = 0;
    bool rid = false; // ASTM Remote ID (service data 0xFFFA)
    char name[WD_NAME_MAX + 1] = "";
};

// AD structures del advertising crudo (cpos_ble_adv_t.data)
static void wd_parse_adv(const uint8_t *d, uint8_t len, WdBleObs &o) {
    o = WdBleObs();
    o.payload_len = len;
    for (uint8_t k = 0; k + 1 < len;) {
        uint8_t l = d[k];
        if (!l || k + 1 + l > len) break;
        uint8_t t = d[k + 1];
        const uint8_t *v = d + k + 2;
        uint8_t vl = (uint8_t)(l - 1);
        if (t == 0xFF && vl >= 2) { o.company = (uint16_t)(v[0] | v[1] << 8); if (vl >= 3) o.mfg_type = v[2]; }
        else if (t == 0x0A && vl >= 1) o.tx = (int8_t)v[0];
        else if ((t == 0x08 || t == 0x09) && (t == 0x09 || !*o.name)) wd_sanitize_name(v, vl, o.name);
        else if (t == 0x02 || t == 0x03) {
            for (uint8_t j = 0; j + 1 < vl && o.nuuids < WD_UUIDS; j += 2) {
                uint16_t u = (uint16_t)(v[j] | v[j + 1] << 8);
                uint8_t p = o.nuuids++;
                while (p && o.uuids[p - 1] > u) { o.uuids[p] = o.uuids[p - 1]; p--; } // orden
                o.uuids[p] = u;
            }
        } else if (t == 0x16 && vl >= 2 && (v[0] | v[1] << 8) == 0xFFFA) o.rid = true;
        k = (uint8_t)(k + 1 + l);
    }
}

static inline void wd_mix(uint64_t &h, uint8_t b) { h ^= b; h *= 1099511628211ULL; }
static bool wd_ble_weak(const WdBleObs &o) { return o.company == 0xFFFF && !o.nuuids && !o.rid; }

static uint64_t wd_ble_hash(const WdBleObs &o) {
    uint64_t h = 14695981039346656037ULL;
    wd_mix(h, o.company & 0xFF); wd_mix(h, o.company >> 8);
    wd_mix(h, o.mfg_type);
    wd_mix(h, o.payload_len);
    wd_mix(h, (uint8_t)(o.tx == WD_NONE8 ? WD_NONE8 : o.tx / 4));
    wd_mix(h, 0); // adTypesPresentMask, 0 en v1 como en Sentinel
    wd_mix(h, o.rid ? 1 : 0);
    for (uint8_t k = 0; k < o.nuuids; k++) { wd_mix(h, o.uuids[k] & 0xFF); wd_mix(h, o.uuids[k] >> 8); }
    return h;
}

// Identidad que va al CSV. Con un payload pobre (sin manufacturer data, UUIDs ni Remote ID) el hash
// de Sentinel no distingue dispositivos: se mezcla la MAC y se marca "ble:w:" (débil) para que el GIS
// sepa que esa identidad no sobrevive a una rotación de MAC. Las demás no dependen de la MAC.
static void wd_ble_identity(const WdBleObs &o, const uint8_t mac[6], uint64_t &key, char *out) {
    bool weak = wd_ble_weak(o);
    key = wd_ble_hash(o);
    if (weak) for (int k = 0; k < 6; k++) wd_mix(key, mac[k]);
    WdOut w(out, 24);
    w.s(weak ? "ble:w:" : "ble:");
    w.hex(key, 16);
}

static void wd_ble_category(uint16_t company, uint8_t mfg_type, bool rid, bool weak, char *out) {
    static const struct { uint16_t id; const char *n; } T[] = {
        {0x004C, "apple"}, {0x0006, "microsoft"}, {0x0075, "samsung"}, {0x00E0, "google"}, {0x0087, "garmin"},
        {0x0059, "nordic"}, {0x01AB, "meta"}, {0x02E5, "espressif"}, {0x012D, "sony"},
    };
    WdOut w(out, 16);
    if (rid) { w.s("drone-rid"); return; }
    if (company == 0x004C && mfg_type == 0x12) { w.s("findmy"); return; }
    if (company != 0xFFFF) {
        for (auto &e : T) if (e.id == company) { w.s("mfg:"); w.s(e.n); return; }
        w.s("mfg:"); w.hex(company, 4);
        return;
    }
    w.s(weak ? "unknown-weak" : "svc");
}

// ---- tablas y ring ---------------------------------------------------------------------------------
// Compactas a propósito (presupuesto de RAM de la app): la identidad y la categoría se recalculan desde
// la clave, y el ring guarda referencias a la tabla más lo propio de cada fila (hora, RSSI, GPS, MAC).
enum { WD_BLE_WEAK = 1, WD_BLE_RID = 2 };
#define WD_LORA_RAW (WD_PAYLOAD_HEX / 2)

struct WdBleEnt {
    uint64_t key;
    uint8_t mac[6];          // la última vista: dato efímero
    uint16_t company;
    uint8_t mfg_type, flags;
    int8_t rmin, rmax;
    uint16_t an;             // observaciones desde la última fila
    int32_t rsum, asum;
    uint32_t n;              // advertisings vistos en total
    uint32_t first_ms, last_ms;
    char name[WD_NAME_MAX + 1];
};

struct WdLoraEnt {
    uint64_t key;
    uint32_t freq_hz;
    int16_t rmin, rmax;
    uint16_t an;
    uint8_t plen;
    uint8_t payload[WD_LORA_RAW]; // primeros bytes del último paquete, crudos (se escriben en hex)
    int32_t rsum, asum, ssum;
    uint32_t n, first_ms, last_ms;
};

struct WdRow {
    uint32_t ts_ms;
    uint8_t source, idx;
    int16_t rssi;
    int8_t snr4;
    uint8_t mac[6];
    WdGps gps;
};

static void wd_ble_ident_str(const WdBleEnt &e, char *out) {
    WdOut w(out, 24);
    w.s(e.flags & WD_BLE_WEAK ? "ble:w:" : "ble:");
    w.hex(e.key, 16);
}
static void wd_lora_ident_str(const WdLoraEnt &e, char *out) {
    WdOut w(out, 24);
    w.s("lora:");
    w.hex(e.key, 16);
}

struct WdStore {
    WdBleEnt ble[WD_MAX_BLE];
    WdLoraEnt lora[WD_MAX_LORA];
    uint8_t nble = 0, nlora = 0;
    WdRow ring[WD_RING];
    uint8_t ring_n = 0;
    uint16_t sweep_at = 0;
    uint32_t drop_ble = 0, drop_lora = 0, drop_ring = 0; // descartados por límite (visibles en la UI)
    uint32_t rows = 0;

    void reset() { nble = nlora = ring_n = 0; sweep_at = 0; drop_ble = drop_lora = drop_ring = rows = 0; }
    bool ring_full() const { return ring_n >= WD_RING; }

    WdRow *push(uint32_t now, uint8_t src, uint8_t idx, const WdGps &g) {
        if (ring_full()) { drop_ring++; return nullptr; }
        WdRow &r = ring[ring_n++];
        memset(&r, 0, sizeof(r));
        r.ts_ms = now; r.source = src; r.idx = idx; r.gps = g;
        r.rssi = WD_NONE16; r.snr4 = WD_NONE8;
        rows++;
        return &r;
    }

    void ble_row(uint8_t i, uint32_t now, const WdGps &g) {
        WdBleEnt &e = ble[i];
        WdRow *r = push(now, WD_SRC_BLE, i, g);
        if (!r) return;
        memcpy(r->mac, e.mac, 6);
        r->rssi = (int16_t)(e.an ? wd_rdiv(e.asum, e.an) : e.rmax); // promedio de la ventana
        e.asum = 0; e.an = 0;
    }

    void lora_row(uint8_t i, uint32_t now, const WdGps &g) {
        WdLoraEnt &e = lora[i];
        WdRow *r = push(now, WD_SRC_LORA, i, g);
        if (!r) return;
        int32_t an = e.an ? e.an : 1;
        r->rssi = (int16_t)wd_rdiv(e.asum, an);
        r->snr4 = (int8_t)wd_rdiv(e.ssum, an);
        e.asum = e.ssum = 0; e.an = 0;
    }

    // Fila k del ring como muestra completa. Nombre/categoría/payload salen de la tabla al vaciar el lote
    // (el payload LoRa es el último paquete de esa identidad en ese momento).
    void sample(uint8_t k, WdSample &m) const {
        const WdRow &r = ring[k];
        m = WdSample();
        m.ts_ms = r.ts_ms; m.source = r.source; m.gps = r.gps; m.rssi = r.rssi; m.snr4 = r.snr4;
        if (r.source == WD_SRC_BLE) {
            const WdBleEnt &e = ble[r.idx];
            wd_ble_ident_str(e, m.identity);
            wd_mac_str(r.mac, m.addr);
            memcpy(m.name, e.name, sizeof(m.name));
            wd_ble_category(e.company, e.mfg_type, e.flags & WD_BLE_RID, e.flags & WD_BLE_WEAK, m.category);
        } else {
            const WdLoraEnt &e = lora[r.idx];
            wd_lora_ident_str(e, m.identity);
            m.freq_hz = e.freq_hz;
            wd_hex(e.payload, e.plen, m.payload, WD_PAYLOAD_HEX);
            wd_copy(m.category, "lora-rx", sizeof(m.category));
        }
    }

    // Un advertising BLE. Una identidad nueva escribe su primera fila enseguida; después se agregan
    // hasta el barrido (sweep). Devuelve el índice en la tabla o -1 si la tabla está llena.
    int ble_observe(const WdBleObs &o, const uint8_t mac[6], int8_t rssi, uint32_t now, const WdGps &g, bool rows_on) {
        uint64_t key;
        char ident[24];
        wd_ble_identity(o, mac, key, ident);
        int i = 0;
        while (i < nble && ble[i].key != key) i++;
        bool fresh = i == nble;
        if (fresh) {
            if (nble >= WD_MAX_BLE) { drop_ble++; return -1; }
            WdBleEnt &e = ble[nble++];
            memset(&e, 0, sizeof(e));
            e.key = key;
            e.company = o.company; e.mfg_type = o.mfg_type;
            e.flags = (uint8_t)((wd_ble_weak(o) ? WD_BLE_WEAK : 0) | (o.rid ? WD_BLE_RID : 0));
            e.rmin = e.rmax = rssi;
            e.first_ms = now;
        }
        WdBleEnt &e = ble[i];
        memcpy(e.mac, mac, 6);
        if (*o.name) memcpy(e.name, o.name, sizeof(e.name)); // el nombre no viene en todos los paquetes
        if (rssi < e.rmin) e.rmin = rssi;
        if (rssi > e.rmax) e.rmax = rssi;
        e.rsum += rssi; e.n++;
        if (e.an < 0xFFFF) { e.asum += rssi; e.an++; }
        e.last_ms = now;
        if (fresh && rows_on) ble_row((uint8_t)i, now, g);
        return i;
    }

    // Un paquete LoRa recibido. La clave agrupa por frecuencia y los primeros 8 bytes crudos del paquete,
    // tomados como bytes opacos: no se decodifica ni interpreta nada del protocolo.
    int lora_observe(uint32_t freq, int16_t rssi, int8_t snr4, const uint8_t *data, uint8_t len, uint32_t now, const WdGps &g, bool rows_on) {
        uint64_t key = 14695981039346656037ULL;
        for (int k = 0; k < 4; k++) wd_mix(key, (uint8_t)(freq >> (k * 8)));
        for (uint8_t k = 0; k < len && k < 8; k++) wd_mix(key, data[k]);
        int i = 0;
        while (i < nlora && lora[i].key != key) i++;
        bool fresh = i == nlora;
        if (fresh) {
            if (nlora >= WD_MAX_LORA) { drop_lora++; return -1; }
            WdLoraEnt &e = lora[nlora++];
            memset(&e, 0, sizeof(e));
            e.key = key; e.freq_hz = freq;
            e.rmin = e.rmax = rssi;
            e.first_ms = now;
        }
        WdLoraEnt &e = lora[i];
        e.plen = len > WD_LORA_RAW ? WD_LORA_RAW : len;
        memcpy(e.payload, data, e.plen);
        if (rssi < e.rmin) e.rmin = rssi;
        if (rssi > e.rmax) e.rmax = rssi;
        e.rsum += rssi; e.n++;
        if (e.an < 0xFFFF) { e.asum += rssi; e.ssum += snr4; e.an++; }
        e.last_ms = now;
        if (fresh && rows_on) lora_row((uint8_t)i, now, g);
        return i;
    }

    // Una fila agregada por cada identidad con observaciones desde su última fila. true = terminó;
    // false = el ring se llenó: vaciarlo y volver a llamar (sigue desde donde quedó).
    bool sweep(uint32_t now, const WdGps &g, bool rows_on) {
        for (; sweep_at < nble + nlora; sweep_at++) {
            bool ble_side = sweep_at < nble;
            uint8_t i = (uint8_t)(ble_side ? sweep_at : sweep_at - nble);
            uint16_t an = ble_side ? ble[i].an : lora[i].an;
            if (!an) continue;
            if (!rows_on) { // sin logging: se cierra la ventana sin fila
                if (ble_side) { ble[i].asum = 0; ble[i].an = 0; }
                else { lora[i].asum = lora[i].ssum = 0; lora[i].an = 0; }
                continue;
            }
            if (ring_full()) return false;
            if (ble_side) ble_row(i, now, g);
            else lora_row(i, now, g);
        }
        sweep_at = 0;
        return true;
    }
};

// Resumen de una identidad BLE para el JSON de metadatos (min/max/promedio de RSSI, primera/última vez)
static int wd_format_ble_summary(const WdBleEnt &e, char *out, size_t cap) {
    WdOut w(out, cap);
    char t[24];
    wd_ble_ident_str(e, t);
    w.s("{\"identity\":"); wd_json_str(w, t);
    wd_ble_category(e.company, e.mfg_type, e.flags & WD_BLE_RID, e.flags & WD_BLE_WEAK, t);
    w.s(",\"category\":"); wd_json_str(w, t);
    w.s(",\"name\":"); if (*e.name) wd_json_str(w, e.name); else w.s("null");
    w.s(",\"rssi_min\":"); w.i(e.rmin);
    w.s(",\"rssi_max\":"); w.i(e.rmax);
    w.s(",\"rssi_avg\":"); w.frac(e.n ? (int64_t)e.rsum * 10 / (int64_t)e.n : 0, 10, 1);
    w.s(",\"adverts\":"); w.u(e.n);
    w.s(",\"first_ms\":"); w.u(e.first_ms);
    w.s(",\"last_ms\":"); w.u(e.last_ms);
    w.c('}');
    return w.ovf ? -1 : (int)w.n;
}
