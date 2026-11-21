// Wardriving pasivo multirradio para Cardputer-ADV + Cap LoRa-1262.
//
// SOLO ESCUCHA. La app nunca transmite, anuncia, conecta, asocia, autentica, desautentica ni descifra:
//   - BLE: observer con scan pasivo (scan_start active=0: no manda SCAN_REQ). No llama connect, ni GATT,
//     ni el servidor GATT (ble_srv), así que el OS no anuncia nada.
//   - WiFi: backend `unsupported`. La ABI 1.8 solo tiene el scan de estación de net.scan_start, que es un
//     scan ACTIVO (manda probe requests) y no da BSSID: no sirve para escuchar beacons/probes sin
//     transmitir, así que no se usa. La app apaga el WiFi del OS (net.mode(0)) mientras corre para que el
//     equipo no quede asociado a la red guardada. Ver README "Extensión de ABI necesaria".
//   - LoRa: solo recepción en un canal fijo y visible (LORA_CFG). Nunca se llama lora.send; los paquetes
//     ajenos se registran como hex truncado sin interpretar ni retransmitir.
//   - GNSS: lectura NMEA (gnss.h de apps/meshtastic). gnss_write no se usa.
// test/core_test.cpp revisa este archivo y falla si aparece alguna llamada que transmita o conecte.
//
// Teclas: enter iniciar/pausar · b BLE · l LoRa · g GPS · r nueva sesión · s guardar y cerrar
//         h (o ?) ayuda -> diagnóstico -> dashboard · ` (esc) volver al dashboard · w diagnóstico WiFi
// La pantalla (dashboard, ayuda, diagnóstico) se arma en wd_ui.h; acá solo se juntan los estados.
//
// SD: /sdcard/wardriving/sessions/session-XXXX.csv.part (append-only, lotes cada 5 s o 32 filas, fflush
// y cierre por lote). Al cierre limpio: .part -> .csv, GeoJSON y JSON de metadatos. Nunca borra nada.

#include <cpos/abi.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern const cpos_api_t *cpos_api;
static inline uint32_t millis() { return cpos_api->millis(); }
#include "gnss.h" // lector NMEA compartido con Meshtastic (apps/meshtastic/src)
#include "wd_core.h"
#include "wd_ui.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#define WD_DIR "/sdcard/wardriving"
#define WD_SESS_DIR WD_DIR "/sessions"
#define FLUSH_MS 5000

// Canal LoRa de escucha: 915.000 MHz (banda ISM de Argentina), BW 125 kHz, SF9, CR 4/5, sync 0x12
// (privado). El SX1262 escucha un canal por vez: solo se ven paquetes con estos mismos parámetros.
// power_dbm al mínimo aunque no se transmita nunca.
static const cpos_lora_cfg_t LORA_CFG = {915000000u, 125000u, 9, 5, 0x12, -9, 8, 1, 0};

enum { R_OFF = WD_RS_OFF, R_ON = WD_RS_ON, R_FAIL = WD_RS_ERROR, R_UNSUPPORTED };

static WdStore S;
static bool armed[3] = {true, false, true};
static uint8_t rstate[3] = {R_OFF, R_UNSUPPORTED, R_OFF};
static char rdiag[3][32] = {"", "ABI 1.8 sin monitor pasivo", ""};
static bool running = false, gps_on = true;
static uint32_t adv_seen = 0, scan_rsp_ignored = 0, lora_seen = 0, drop_line = 0; // ev de la sesión
static uint32_t ble_last_ms = 0, lora_last_ms = 0; // 0 = sin eventos todavía
static uint32_t run_acc_ms = 0, run_start_ms = 0;  // tiempo capturando en la sesión
static uint32_t gps_on_at = 0;
static bool gps_had_fix = false, sd_error = false;
static uint8_t view = WD_VIEW_DASH;
static uint32_t toast_at = 0;

static int sess_no = 0;
static bool sess_open = false, log_ok = false, meta_has_boot = false;
static char sd_msg[34] = "";
static uint64_t boot_unix_ms = 0;
static uint32_t last_flush = 0, last_draw = 0, last_wifi_off = 0;
static uint32_t rec_rows = 0, rec_bad = 0, rec_torn = 0; // última recuperación/exportación
static uint8_t recovered = 0;
static char diag[WD_UI_COLS + 1] = ""; // aviso corto de la última tecla

static char g_line[WD_LINE_MAX];
static char g_feat[WD_FEAT_MAX];

static void say(const char *m) { strncpy(diag, m, sizeof(diag) - 1); diag[sizeof(diag) - 1] = 0; toast_at = millis(); }

// ---- GNSS -----------------------------------------------------------------------------------------
static WdGps gpsNow() {
    WdGps g;
    uint32_t age = millis() - G.fix_at;
    g.sats = G.sats;
    g.valid = G.up && G.fix && age <= WD_FIX_MAX_AGE_MS;
    if (g.valid) {
        g.lat_e7 = G.lat; g.lon_e7 = G.lon;
        g.alt_m = G.alt; g.has_alt = G.sats > 0;
        if (G.hdop > 0 && G.hdop < 999) g.acc_dm = (uint16_t)(G.hdop * 50 + 0.5f); // HDOP x 5 m (estimado)
        g.fix_age_ms = age;
        gps_had_fix = true;
    }
    if (G.unix && !boot_unix_ms) boot_unix_ms = (uint64_t)G.unix * 1000 - G.unix_at;
    return g;
}

// ---- radios (solo recepción) ----------------------------------------------------------------------
static void wifiQuiet() {
    // El launcher puede haber dejado el WiFi asociado a la red guardada: se apaga (no transmite nada,
    // libera ~50 KB). n_mode(0) falla mientras el OS se está conectando en segundo plano: se reintenta.
    cpos_api->net.mode(0);
}

static void bleSet(bool on) {
    if (on && rstate[WD_SRC_BLE] != R_ON) {
        if (cpos_api->ble.begin() != 1) { rstate[WD_SRC_BLE] = R_FAIL; strcpy(rdiag[WD_SRC_BLE], "begin: sin stack BLE (RAM?)"); return; }
        // ms 0 = hasta stop; active 0 = PASIVO (sin SCAN_REQ); sin filtro de duplicados para seguir el RSSI
        if (!cpos_api->ble.scan_start(0, 0, 100, 90, 0)) { cpos_api->ble.end(); rstate[WD_SRC_BLE] = R_FAIL; strcpy(rdiag[WD_SRC_BLE], "scan_start fallo"); return; }
        rstate[WD_SRC_BLE] = R_ON;
        strcpy(rdiag[WD_SRC_BLE], "observer pasivo");
    } else if (!on && rstate[WD_SRC_BLE] == R_ON) {
        cpos_api->ble.scan_stop();
        cpos_api->ble.end();
        rstate[WD_SRC_BLE] = R_OFF;
    }
}

static void loraSet(bool on) {
    if (on && rstate[WD_SRC_LORA] != R_ON) {
        int r = cpos_api->lora.begin(&LORA_CFG);
        if (r) {
            rstate[WD_SRC_LORA] = R_FAIL;
            strcpy(rdiag[WD_SRC_LORA], r == -1 ? "sin permiso" : r == -2 ? "no responde el SX1262 (hat?)" : r == -3 ? "config invalida" : "begin fallo");
            return;
        }
        rstate[WD_SRC_LORA] = R_ON;
        strcpy(rdiag[WD_SRC_LORA], "solo RX, canal fijo");
    } else if (!on && rstate[WD_SRC_LORA] == R_ON) {
        cpos_api->lora.end();
        rstate[WD_SRC_LORA] = R_OFF;
    }
}

static void radiosApply() {
    bleSet(running && armed[WD_SRC_BLE]);
    loraSet(running && armed[WD_SRC_LORA]);
}

// ---- SD -------------------------------------------------------------------------------------------
static void sessPath(char *out, size_t n, int no, const char *ext) { snprintf(out, n, WD_SESS_DIR "/session-%04d%s", no, ext); }
static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static bool rowsOn() { return running && sess_open && log_ok; }

static void sdFail(const char *what) {
    log_ok = false; // se deja de escribir; la captura sigue en RAM (tablas y ring)
    sd_error = true;
    snprintf(sd_msg, sizeof(sd_msg), "%s", what);
}

static void flushRing() {
    if (!S.ring_n || !sess_open || !log_ok) return;
    char p[64];
    sessPath(p, sizeof(p), sess_no, ".csv.part");
    FILE *f = fopen(p, "ab");
    if (!f) { sdFail("abrir .part"); return; }
    bool ok = true;
    static WdSample m;
    for (uint8_t k = 0; k < S.ring_n && ok; k++) {
        S.sample(k, m);
        int n = wd_format_csv(m, g_line, sizeof(g_line));
        if (n < 0) { drop_line++; continue; }
        ok = fwrite(g_line, 1, (size_t)n, f) == (size_t)n;
    }
    if (fflush(f)) ok = false;
    if (fclose(f)) ok = false;
    if (!ok) { sdFail("escribir lote"); return; }
    S.ring_n = 0;
}

static void writeMeta(const char *state) {
    char p[64];
    sessPath(p, sizeof(p), sess_no, ".json");
    FILE *f = fopen(p, "wb");
    if (!f) return;
    char csvn[32], geon[32];
    snprintf(csvn, sizeof(csvn), "session-%04d.csv", sess_no);
    snprintf(geon, sizeof(geon), "session-%04d.geojson", sess_no);
    bool closed = !strcmp(state, "closed");
    fprintf(f, "{\"app\":\"wardriving\",\"version\":\"%s\",\"session\":%d,\"state\":\"%s\",\n", APP_VERSION, sess_no, state);
    fprintf(f, "\"passive_only\":true,\"transmits\":false,\n");
    fprintf(f, "\"csv\":\"%s%s\",\"geojson\":%s%s%s,\n", csvn, closed ? "" : ".part", closed ? "\"" : "", closed ? geon : "null", closed ? "\"" : "");
    fprintf(f, "\"columns\":\"%s\",\n", WD_CSV_HEADER);
    fprintf(f, "\"timestamp_ms\":\"millis desde el arranque del equipo\",\"boot_unix_ms\":");
    if (boot_unix_ms) fprintf(f, "%lu%03lu", (unsigned long)(boot_unix_ms / 1000), (unsigned long)(boot_unix_ms % 1000));
    else fprintf(f, "null");
    fprintf(f, ",\n\"crs\":\"WGS84 (EPSG:4326), grados decimales\",\"fix_max_age_ms\":%d,\"accuracy_m\":\"HDOP x 5 m (estimado)\",\n", WD_FIX_MAX_AGE_MS);
    fprintf(f, "\"sources\":{\"ble\":{\"armed\":%s,\"backend\":\"observer, scan pasivo (sin SCAN_REQ)\"},\n", armed[0] ? "true" : "false");
    fprintf(f, "\"wifi\":{\"armed\":%s,\"backend\":\"unsupported\",\"reason\":\"ABI 1.8: solo scan STA activo (probe requests) sin BSSID; falta modo promiscuo\"},\n", armed[1] ? "true" : "false");
    fprintf(f, "\"lora\":{\"armed\":%s,\"backend\":\"solo RX\",\"freq_hz\":%lu,\"bw_hz\":%lu,\"sf\":%u,\"cr\":\"4/%u\",\"sync_word\":%u,\"tx\":false}},\n",
            armed[2] ? "true" : "false", (unsigned long)LORA_CFG.freq_hz, (unsigned long)LORA_CFG.bw_hz, LORA_CFG.sf, LORA_CFG.cr, LORA_CFG.sync_word);
    fprintf(f, "\"limits\":{\"ble\":%d,\"wifi\":%d,\"lora\":%d,\"ring\":%d,\"payload_hex\":%d,\"name\":%d,\"line\":%d},\n",
            WD_MAX_BLE, WD_MAX_WIFI, WD_MAX_LORA, WD_RING, WD_PAYLOAD_HEX, WD_NAME_MAX, WD_LINE_MAX);
    fprintf(f, "\"counts\":{\"rows\":%lu,\"ble_ids\":%u,\"lora_ids\":%u,\"ble_adverts\":%lu,\"lora_packets\":%lu,"
               "\"dropped_ble_ids\":%lu,\"dropped_lora_ids\":%lu,\"dropped_rows\":%lu,\"dropped_lines\":%lu},\n",
            (unsigned long)S.rows, S.nble, S.nlora, (unsigned long)adv_seen, (unsigned long)lora_seen, (unsigned long)S.drop_ble,
            (unsigned long)S.drop_lora, (unsigned long)S.drop_ring, (unsigned long)drop_line);
    if (closed) fprintf(f, "\"export\":{\"rows\":%lu,\"bad\":%lu,\"torn\":%lu},\n", (unsigned long)rec_rows, (unsigned long)rec_bad, (unsigned long)rec_torn);
    fprintf(f, "\"ble_identities\":[");
    for (uint8_t k = 0; k < S.nble; k++) {
        if (wd_format_ble_summary(S.ble[k], g_feat, sizeof(g_feat)) < 0) continue;
        fprintf(f, "%s\n%s", k ? "," : "", g_feat);
    }
    fprintf(f, "]}\n");
    fclose(f);
    meta_has_boot = boot_unix_ms != 0;
}

// Lee un CSV de esta app (o un .part cortado) registro por registro y escribe las filas válidas: como CSV
// (si csv_out) y/o como GeoJSON (si geo_out, en geo_out.tmp y renombrado al final, así nunca queda un
// GeoJSON a medias con el nombre final). Cuenta filas, inválidas y registro cortado al final.
static bool exportRows(const char *src, const char *csv_out, const char *geo_out) {
    rec_rows = rec_bad = rec_torn = 0;
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    char ctmp[72], gtmp[72];
    FILE *co = nullptr, *go = nullptr;
    bool ok = true;
    if (csv_out) { snprintf(ctmp, sizeof(ctmp), "%s.tmp", csv_out); co = fopen(ctmp, "wb"); ok = co && fputs(WD_CSV_HEADER "\n", co) >= 0; }
    if (ok && geo_out) { snprintf(gtmp, sizeof(gtmp), "%s.tmp", geo_out); go = fopen(gtmp, "wb"); ok = go && fputs(WD_GEOJSON_HEAD, go) >= 0; }
    static WdRecAsm ra;
    ra = WdRecAsm();
    WdSample m;
    char *fl[WD_NFIELDS + 1];
    while (ok && fgets(g_line, sizeof(g_line), in)) {
        if (!ra.feed(g_line)) continue;
        if (!strcmp(ra.buf, WD_CSV_HEADER "\n") || !strcmp(ra.buf, WD_CSV_HEADER "\r\n")) continue;
        int nf = wd_csv_split(ra.buf, fl, WD_NFIELDS + 1);
        if (!wd_sample_from_fields(fl, nf, m)) { rec_bad++; continue; }
        if (co) { int n = wd_format_csv(m, g_feat, sizeof(g_feat)); ok = n > 0 && fwrite(g_feat, 1, (size_t)n, co) == (size_t)n; }
        if (ok && go) {
            int n = wd_format_feature(m, g_feat, sizeof(g_feat));
            if (n < 0) { rec_bad++; continue; }
            ok = (!rec_rows || fputs(",\n", go) >= 0) && fwrite(g_feat, 1, (size_t)n, go) == (size_t)n;
        }
        rec_rows++;
    }
    rec_bad += ra.too_long;
    if (ra.leftover()) rec_torn = 1;
    fclose(in);
    if (go) { ok = ok && fputs(WD_GEOJSON_TAIL, go) >= 0 && fflush(go) == 0; ok = fclose(go) == 0 && ok; }
    if (co) { ok = ok && fflush(co) == 0; ok = fclose(co) == 0 && ok; }
    if (!ok) return false; // los .tmp quedan en la SD; no se borra nada
    if (co && rename(ctmp, csv_out)) return false;
    if (go && rename(gtmp, geo_out)) return false;
    return true;
}

static bool sdDirs() {
    mkdir(WD_DIR, 0777);
    mkdir(WD_SESS_DIR, 0777);
    return exists(WD_SESS_DIR);
}

// Siguiente número de sesión, y recuperación de los .part que quedaron sin cerrar (corte de energía):
// se genera session-XXXX.csv con las filas completas y el GeoJSON; el .part queda intacto.
static int scanSessions(bool recover) {
    int maxno = 0;
    int orphans[8], no = 0;
    DIR *d = opendir(WD_SESS_DIR);
    if (!d) return 1;
    while (struct dirent *e = readdir(d)) {
        const char *nm = e->d_name;
        if (strncmp(nm, "session-", 8) || strlen(nm) < 12) continue;
        int n = atoi(nm + 8);
        if (n > maxno) maxno = n;
        if (recover && !strcmp(nm + 12, ".csv.part") && no < 8) orphans[no++] = n;
    }
    closedir(d);
    for (int k = 0; k < no; k++) {
        char part[64], csv[64], geo[64];
        sessPath(part, sizeof(part), orphans[k], ".csv.part");
        sessPath(csv, sizeof(csv), orphans[k], ".csv");
        sessPath(geo, sizeof(geo), orphans[k], ".geojson");
        if (exists(csv)) continue; // ya recuperado antes (o cerrado y el .part es otro): no tocar
        if (exportRows(part, csv, exists(geo) ? nullptr : geo)) recovered++;
    }
    return maxno + 1 > 9999 ? 9999 : maxno + 1;
}

static void openSession() {
    S.reset();
    adv_seen = lora_seen = drop_line = 0;
    ble_last_ms = lora_last_ms = 0;
    run_acc_ms = 0; run_start_ms = millis();
    sd_error = false;
    if (!sdDirs()) { sess_open = false; log_ok = false; strcpy(sd_msg, "sin SD: captura solo en RAM"); return; }
    sess_no = scanSessions(false);
    char p[64];
    sessPath(p, sizeof(p), sess_no, ".csv.part");
    if (exists(p)) { sdFail("la sesion ya existe"); return; }
    FILE *f = fopen(p, "wb");
    if (!f) { sess_open = false; sdFail("crear .part"); return; }
    bool ok = fputs(WD_CSV_HEADER "\n", f) >= 0 && fflush(f) == 0;
    ok = fclose(f) == 0 && ok;
    sess_open = true;
    log_ok = ok;
    if (!ok) { sdFail("encabezado"); return; }
    snprintf(sd_msg, sizeof(sd_msg), "ultimo lote ok");
    writeMeta("open");
    last_flush = millis();
}

// barrido de ventanas + vaciado del ring
static void batch(uint32_t now) {
    WdGps g = gpsNow();
    while (!S.sweep(now, g, rowsOn())) {
        flushRing();
        if (S.ring_full()) break; // la SD falló: rowsOn() ya es false, el próximo barrido no escribe
    }
    flushRing();
    if (sess_open && log_ok && boot_unix_ms && !meta_has_boot) writeMeta("open");
}

static void closeSession() {
    if (!sess_open) return;
    batch(millis());
    char part[64], csv[64], geo[64];
    sessPath(part, sizeof(part), sess_no, ".csv.part");
    sessPath(csv, sizeof(csv), sess_no, ".csv");
    sessPath(geo, sizeof(geo), sess_no, ".geojson");
    if (log_ok && !exists(csv) && rename(part, csv) == 0) {
        bool g = exportRows(csv, nullptr, geo);
        writeMeta("closed");
        snprintf(sd_msg, sizeof(sd_msg), g ? "cerrada %04d: %lu filas" : "cerrada %04d, fallo GeoJSON", sess_no, (unsigned long)rec_rows);
    } else {
        writeMeta("open"); // el .part sigue siendo la fuente de verdad; se recupera al próximo arranque
        snprintf(sd_msg, sizeof(sd_msg), "sin cerrar %04d: queda el .part", sess_no);
    }
    sess_open = false;
    log_ok = false;
}

// ---- captura ----------------------------------------------------------------------------------
static void pollBle(uint32_t now) {
    if (rstate[WD_SRC_BLE] != R_ON) return;
    static cpos_ble_adv_t a;
    static WdBleObs o;
    WdGps g = gpsNow();
    for (int k = 0; k < 48 && cpos_api->ble.scan_read(&a); k++) {
        adv_seen++;
        ble_last_ms = now ? now : 1;
        // Un SCAN_RSP tiene otro largo y otros AD: no entra al fingerprint (con scan pasivo no debería llegar)
        if (a.event == 4) { scan_rsp_ignored++; continue; }
        wd_parse_adv(a.data, a.len > 31 ? 31 : a.len, o);
        if (S.ring_full()) flushRing();
        S.ble_observe(o, a.addr, a.rssi, now, g, rowsOn());
    }
}

static void pollLora(uint32_t now) {
    if (rstate[WD_SRC_LORA] != R_ON) return;
    static cpos_lora_pkt_t p;
    WdGps g = gpsNow();
    for (int k = 0; k < 8 && cpos_api->lora.recv(&p); k++) {
        lora_seen++;
        lora_last_ms = now ? now : 1;
        if (S.ring_full()) flushRing();
        S.lora_observe(LORA_CFG.freq_hz, p.rssi, p.snr4, p.data, p.len, p.ms ? p.ms : now, g, rowsOn());
    }
}

// ---- teclado y pantalla -------------------------------------------------------------------------
static void setRunning(bool on) {
    if (on == running) return;
    uint32_t now = millis();
    if (on && !sess_open) openSession();
    if (!on) { batch(now); run_acc_ms += now - run_start_ms; }
    else run_start_ms = now;
    running = on;
    radiosApply();
    say(on ? "Captura activa: solo escucha" : "Captura pausada");
    if (!on && sess_open && log_ok) writeMeta("open");
}

static void onKey(char c) {
    switch (c) {
    case 'b': armed[WD_SRC_BLE] = !armed[WD_SRC_BLE]; radiosApply(); say(armed[WD_SRC_BLE] ? "BLE activado" : "BLE apagado"); break;
    case 'l': armed[WD_SRC_LORA] = !armed[WD_SRC_LORA]; radiosApply(); say(armed[WD_SRC_LORA] ? "LoRa activado (solo RX)" : "LoRa apagado"); break;
    case 'w': view = WD_VIEW_DIAG; break; // WiFi no está disponible: el motivo está en el diagnóstico
    case 'g':
        gps_on = !gps_on;
        setGps(gps_on);
        gps_on_at = millis();
        gps_had_fix = false;
        say(gps_on ? "GPS activado" : "GPS apagado");
        break;
    case 'r': { // cierra la sesión actual (limpio) y abre otra; sigue capturando si estaba capturando
        bool was = running;
        setRunning(false);
        closeSession();
        openSession();
        setRunning(was);
        say("Nueva sesion");
        break;
    }
    case 's': setRunning(false); closeSession(); say("Sesion guardada y cerrada"); break;
    case 'h': case '?': view = view == WD_VIEW_DASH ? WD_VIEW_HELP : view == WD_VIEW_HELP ? WD_VIEW_DIAG : WD_VIEW_DASH; break;
    case '`': view = WD_VIEW_DASH; break; // esc
    }
}

static void pollKeys() {
    static uint32_t seq = 0;
    static char prev[CPOS_KBD_MAX_KEYS + 1] = "";
    static bool prev_enter = false;
    cpos_kbd_t k;
    cpos_api->kbd_state(&k);
    if (k.seq == seq) return;
    seq = k.seq;
    char now[CPOS_KBD_MAX_KEYS + 1];
    uint8_t n = k.count > CPOS_KBD_MAX_KEYS ? CPOS_KBD_MAX_KEYS : k.count;
    for (uint8_t i = 0; i < n; i++) now[i] = (char)((k.keys[i] >= 'A' && k.keys[i] <= 'Z') ? k.keys[i] + 32 : k.keys[i]);
    now[n] = 0;
    for (uint8_t i = 0; i < n; i++) if (!strchr(prev, now[i])) onKey(now[i]);
    memcpy(prev, now, n + 1);
    if (k.enter && !prev_enter) setRunning(!running);
    prev_enter = k.enter;
}

static WdUiRadio radioUi(uint8_t src, uint32_t ids, uint32_t ev, uint32_t drop, uint32_t last_ms, uint32_t now) {
    WdUiRadio r;
    r.state = rstate[src] == R_ON ? WD_RS_ON : rstate[src] == R_FAIL ? WD_RS_ERROR : WD_RS_OFF;
    r.armed = armed[src];
    r.ids = ids; r.ev = ev; r.drop = drop;
    r.last_age_ms = last_ms ? now - last_ms : WD_NO_AGE;
    r.why = rdiag[src];
    return r;
}

static void draw() {
    uint32_t now = millis();
    WdGps g = gpsNow();
    WdUiIn in;
    in.version = APP_VERSION;
    in.running = running; in.sess_open = sess_open; in.sess_no = sess_no;
    in.elapsed_s = (run_acc_ms + (running ? now - run_start_ms : 0)) / 1000;
    in.sd_error = sd_error; in.log_ok = log_ok; in.sd_msg = sd_msg;
    in.ble = radioUi(WD_SRC_BLE, S.nble, adv_seen, S.drop_ble, ble_last_ms, now);
    in.lora = radioUi(WD_SRC_LORA, S.nlora, lora_seen, S.drop_lora, lora_last_ms, now);
    in.lora_freq_hz = LORA_CFG.freq_hz; in.lora_bw_hz = LORA_CFG.bw_hz; in.lora_sf = LORA_CFG.sf; in.lora_sync = LORA_CFG.sync_word;
    in.gps_on = gps_on; in.gps_port = G.up; in.gps_nmea = G.seen; in.gps_fix = g.valid; in.gps_had_fix = gps_had_fix;
    in.gps_on_ms = now - gps_on_at;
    in.sats = G.sats; in.acc_dm = g.acc_dm;
    in.hdop10 = G.hdop > 0 && G.hdop < 99 ? (uint16_t)(G.hdop * 10 + 0.5f) : 0;
    in.fix_age_ms = g.valid ? g.fix_age_ms : WD_NO_AGE;
    uint32_t last = ble_last_ms > lora_last_ms ? ble_last_ms : lora_last_ms;
    in.last_src = last ? (ble_last_ms >= lora_last_ms ? WD_SRC_BLE : WD_SRC_LORA) : 255;
    in.last_age_ms = last ? now - last : WD_NO_AGE;
    in.rows = S.rows; in.pending = S.ring_n; in.ring = WD_RING;
    in.toast = *diag && now - toast_at < 2500 ? diag : "";

    const cpos_gfx_api_t &x = cpos_api->gfx;
    cpos_canvas_t *c = x.screen(); // el canvas del OS: sin framebuffer propio
    x.set_font(c, CPOS_FONT0);
    x.set_text_size(c, 1, 1);
    wd_ui_render(view, in, [&](const WdUiItem &it) {
        switch (it.kind) {
        case WD_UI_FILL: x.rect(c, it.x, it.y, it.w, it.h, it.fg, 1); break;
        case WD_UI_HLINE: x.hline(c, it.x, it.y, it.w, it.fg); break;
        case WD_UI_DOT: x.circle(c, it.x, it.y, it.w, it.fg, 1); break;
        default: x.set_text_color(c, it.fg, it.bg, 1); x.draw_string(c, it.text, it.x, it.y); break;
        }
    });
}

// ---- Arduino --------------------------------------------------------------------------------------
void setup() {
    cpos_api->gfx.set_brightness(80);
    gps_on_at = millis();
    strcpy(sd_msg, "recuperando sesiones...");
    draw();
    wifiQuiet();
    if (sdDirs()) scanSessions(true);
    if (recovered) snprintf(sd_msg, sizeof(sd_msg), "recuperadas %u sesiones .part", recovered);
    else strcpy(sd_msg, "SD lista");
    setGps(true);
    gps_on_at = millis();
}

void loop() {
    uint32_t now = millis();
    if (cpos_api->exit_requested()) { // salida limpia: .part -> .csv y GeoJSON
        setRunning(false);
        closeSession();
        setGps(false);
        return;
    }
    pollGnss();
    pollKeys();
    if (now - last_wifi_off > 10000) { last_wifi_off = now; wifiQuiet(); } // por si el OS volvió a conectar
    if (running) {
        pollBle(now);
        pollLora(now);
        if (now - last_flush >= FLUSH_MS || S.ring_full()) { last_flush = now; batch(now); }
    }
    if (now - last_draw >= 250) { last_draw = now; draw(); }
    cpos_api->delay_ms(5);
}
