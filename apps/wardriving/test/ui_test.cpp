// Test de host de la interfaz (wd_ui.h): recorre combinaciones de estado y revisa lo que se dibujaría.
//
//   c++ -std=c++17 -Wall -Iapps/wardriving/src apps/wardriving/test/ui_test.cpp -o /tmp/wd_ui_test && \
//       /tmp/wd_ui_test apps/wardriving/src/main.cpp apps/wardriving/src/wd_ui.h
#include "wd_ui.h"
#include <ctype.h>
#include <stdio.h>
#include <string>
#include <vector>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

struct Screen {
    std::vector<WdUiItem> items;
    std::vector<WdUiItem> texts() const { std::vector<WdUiItem> t; for (auto &i : items) if (i.kind == WD_UI_TEXT) t.push_back(i); return t; }
    const WdUiItem *find(const char *s) const { for (auto &i : items) if (i.kind == WD_UI_TEXT && strstr(i.text, s)) return &i; return nullptr; }
    const WdUiItem *exact(const char *s) const { for (auto &i : items) if (i.kind == WD_UI_TEXT && !strcmp(i.text, s)) return &i; return nullptr; }
    bool has(const char *s) const { return find(s) != nullptr; }
};

static Screen render(uint8_t view, const WdUiIn &in) {
    Screen s;
    wd_ui_render(view, in, [&](const WdUiItem &it) { s.items.push_back(it); });
    return s;
}

// palabra completa (no dentro de otra: "PAUSA" no debe saltar en "PAUSADA")
static bool word(const char *text, const char *w) {
    size_t l = strlen(w);
    for (const char *p = strstr(text, w); p; p = strstr(p + 1, w)) {
        bool a = p == text || !isalnum((unsigned char)p[-1]);
        bool b = !isalnum((unsigned char)p[l]);
        if (a && b) return true;
    }
    return false;
}

// todo adentro de 240x135 y sin textos encimados
static void check_layout(const Screen &s, const char *what) {
    auto t = s.texts();
    for (auto &i : t) {
        bool in = i.x >= 0 && i.y >= 0 && i.x + i.w <= WD_UI_W - 1 && i.y + WD_UI_CH <= WD_UI_H && (int)strlen(i.text) * WD_UI_CW == i.w;
        if (!in) printf("  fuera de pantalla (%s): '%s' x=%d y=%d\n", what, i.text, i.x, i.y);
        CHECK(in);
    }
    for (size_t a = 0; a < t.size(); a++)
        for (size_t b = a + 1; b < t.size(); b++) {
            bool rows = t[a].y < t[b].y + WD_UI_CH && t[b].y < t[a].y + WD_UI_CH;
            bool cols = t[a].x < t[b].x + t[b].w + WD_UI_CW && t[b].x < t[a].x + t[a].w + WD_UI_CW; // 1 carácter de aire
            if (rows && cols && *t[a].text && *t[b].text) {
                printf("  encimados (%s): '%s' / '%s'\n", what, t[a].text, t[b].text);
                CHECK(false);
            }
        }
}

static void check_no_banned(const Screen &s, const char *what) {
    const char *banned[] = {"REC", "PAUSA", "on", "off", "ON", "OFF", "On", "Off", "CONECTADO", "unsupported", "armado"};
    for (auto &i : s.texts()) {
        for (const char *b : banned) {
            bool f = word(i.text, b);
            if (f) printf("  palabra prohibida (%s): '%s' en '%s'\n", what, b, i.text);
            CHECK(!f);
        }
        CHECK(!strchr(i.text, '*')); // el asterisco no es un estado
    }
}

static WdUiIn base() {
    WdUiIn in;
    in.version = "0.2.0";
    in.running = true; in.sess_open = true; in.sess_no = 7; in.elapsed_s = 12 * 60 + 48; in.log_ok = true;
    in.sd_msg = "ultimo lote ok";
    in.ble.state = WD_RS_ON; in.ble.armed = true; in.ble.ids = 24; in.ble.ev = 128; in.ble.last_age_ms = 2000;
    in.lora.state = WD_RS_ON; in.lora.armed = true; in.lora.ids = 7; in.lora.ev = 43; in.lora.last_age_ms = 9000;
    in.lora_freq_hz = 915000000; in.lora_bw_hz = 125000; in.lora_sf = 9; in.lora_sync = 0x12;
    in.gps_on = true; in.gps_port = true; in.gps_nmea = true; in.gps_fix = true; in.gps_had_fix = true; in.gps_on_ms = 60000;
    in.sats = 8; in.acc_dm = 50; in.hdop10 = 10; in.fix_age_ms = 400;
    in.last_src = 0; in.last_age_ms = 2000;
    in.rows = 171; in.pending = 12; in.ring = 32;
    return in;
}

static void test_mockup() {
    WdUiIn in = base();
    Screen s = render(WD_VIEW_DASH, in);
    check_layout(s, "maqueta");
    check_no_banned(s, "maqueta");
    CHECK(s.exact("CAPTURA ACTIVA"));
    CHECK(s.exact("00:12:48"));
    CHECK(s.exact("Sesion 0007"));
    const WdUiItem *sd = s.exact("SD: GRABANDO");
    CHECK(sd && sd->fg == WD_C_GREEN);
    // las cuatro filas: nombre, estado textual y contadores con sus etiquetas
    CHECK(s.exact("BLE") && s.exact("WiFi") && s.exact("LoRa") && s.exact("GPS"));
    const WdUiItem *ble = s.exact("ESCUCHANDO");
    CHECK(ble && ble->fg == WD_C_GREEN && ble->y == s.exact("BLE")->y);
    CHECK(s.exact("24 ids 128 ev") && s.exact("7 ids 43 ev"));
    const WdUiItem *nd = s.exact("NO DISP");
    CHECK(nd && nd->fg == WD_C_GREY && s.exact("ABI 1.8"));
    const WdUiItem *fx = s.exact("FIX");
    CHECK(fx && fx->fg == WD_C_GREEN && s.exact("8 sat ~5m"));
    CHECK(s.has("Ultimo: BLE") && s.has("hace 2s"));
    CHECK(s.has("Muestras: 171") && s.has("lote: 12/32"));
    // controles en dos líneas, iguales a las teclas
    const WdUiItem *c1 = s.find("ENTER pausa"), *c2 = s.find("R nueva");
    CHECK(c1 && c2 && c1->y < c2->y && c1->fg == WD_C_CYAN);
    const char *keys1[] = {"ENTER", "B BLE", "L LoRa", "G GPS"}, *keys2[] = {"R nueva", "S guardar", "H ayuda"};
    for (const char *k : keys1) CHECK(c1 && strstr(c1->text, k));
    for (const char *k : keys2) CHECK(c2 && strstr(c2->text, k));
    // encabezado rojo con la captura activa (y el texto lo dice igual)
    bool red_bar = false;
    for (auto &i : s.items) if (i.kind == WD_UI_FILL && i.y == 0 && i.h == 13 && i.fg == WD_C_RED) red_bar = true;
    CHECK(red_bar);
}

static void test_states() {
    // captura: ACTIVA / PAUSADA / DETENIDA, nunca REC/PAUSA
    WdUiIn in = base();
    in.running = false;
    in.ble.state = in.lora.state = WD_RS_OFF; // en pausa main.cpp apaga las radios
    Screen s = render(WD_VIEW_DASH, in);
    CHECK(s.exact("CAPTURA PAUSADA") && s.has("ENTER iniciar"));
    CHECK(s.exact("APAGADO")); // radios apagadas en pausa...
    CHECK(s.exact("listo 24 ids 128 ev")); // ...pero armadas: se ve que arrancan con ENTER
    in.sess_open = false;
    s = render(WD_VIEW_DASH, in);
    CHECK(s.exact("CAPTURA DETENIDA") && s.exact("--:--:--") && s.exact("Sin sesion") && s.exact("SD: SIN SESION"));

    // BLE / LoRa: ESCUCHANDO, APAGADO, ERROR
    in = base();
    in.ble.state = WD_RS_ERROR; in.ble.why = "begin: sin stack BLE (RAM?)";
    in.lora.state = WD_RS_OFF; in.lora.armed = false;
    s = render(WD_VIEW_DASH, in);
    const WdUiItem *e = s.exact("ERROR");
    CHECK(e && e->fg == WD_C_RED && e->y == s.exact("BLE")->y);
    const WdUiItem *off = s.exact("APAGADO");
    CHECK(off && off->fg == WD_C_GREY && off->y == s.exact("LoRa")->y);

    // GPS: FIX solo con fix fresco; puerto abierto no es fix
    in = base();
    in.gps_fix = false; in.gps_had_fix = false;
    CHECK(wd_ui_gps(in) == WD_GPS_BUSCANDO);
    in.gps_had_fix = true;
    CHECK(wd_ui_gps(in) == WD_GPS_SIN_FIX);
    in.gps_nmea = false; in.gps_on_ms = 3000;
    CHECK(wd_ui_gps(in) == WD_GPS_BUSCANDO);
    in.gps_on_ms = WD_GPS_NMEA_TIMEOUT_MS + 1;
    CHECK(wd_ui_gps(in) == WD_GPS_ERROR);
    in.gps_port = false;
    CHECK(wd_ui_gps(in) == WD_GPS_ERROR);
    in.gps_on = false;
    CHECK(wd_ui_gps(in) == WD_GPS_APAGADO);
    const char *gt[] = {"APAGADO", "ERROR", "BUSCANDO", "SIN FIX", "FIX"};
    const uint16_t gc[] = {WD_C_GREY, WD_C_RED, WD_C_YELLOW, WD_C_YELLOW, WD_C_GREEN};
    for (uint8_t k = 0; k < 5; k++) CHECK(!strcmp(wd_ui_gps_txt(k), gt[k]) && wd_ui_gps_color(k) == gc[k]);

    // SD: GRABANDO / RAM / ERROR
    in = base();
    in.log_ok = false; in.sess_open = false;
    s = render(WD_VIEW_DASH, in);
    const WdUiItem *ram = s.exact("SD: RAM");
    CHECK(ram && ram->fg == WD_C_YELLOW);
    in = base();
    in.sd_error = true; in.log_ok = false; in.sd_msg = "escribir lote";
    s = render(WD_VIEW_DASH, in);
    const WdUiItem *err = s.exact("SD: ERROR");
    CHECK(err && err->fg == WD_C_RED);
    Screen d = render(WD_VIEW_DIAG, in);
    const WdUiItem *why = d.find("escribir lote"); // el motivo, en el diagnóstico
    CHECK(why && why->fg == WD_C_RED);

    // aviso de tecla: reemplaza "Ultimo:" un momento, en celeste
    in = base();
    in.toast = "Sesion guardada y cerrada";
    s = render(WD_VIEW_DASH, in);
    CHECK(s.exact("Sesion guardada y cerrada") && !s.has("Ultimo:"));
    in.toast = ""; in.last_src = 255;
    s = render(WD_VIEW_DASH, in);
    CHECK(s.exact("Ultimo: sin eventos"));
}

// todas las combinaciones: nada se corta, nada se encima, sin palabras prohibidas, el estado siempre entero
static void test_all_combinations() {
    const uint32_t counts[] = {0, 7, 9999, 123456, 4000000000u};
    int screens = 0;
    for (int cap = 0; cap < 3; cap++)
        for (int bs = 0; bs < 3; bs++)
            for (int ls = 0; ls < 3; ls++)
                for (int gps = 0; gps < 6; gps++)
                    for (int sd = 0; sd < 3; sd++)
                        for (uint32_t c : counts) {
                            WdUiIn in = base();
                            in.running = cap == 2; in.sess_open = cap >= 1;
                            in.ble.state = (uint8_t)bs; in.lora.state = (uint8_t)ls;
                            in.ble.armed = in.lora.armed = c & 1;
                            in.ble.ids = in.lora.ids = c > 48 ? 48 : c;
                            in.ble.ev = in.lora.ev = c; in.ble.drop = in.lora.drop = c;
                            in.ble.last_age_ms = c * 1000u;
                            in.ble.why = "no responde el SX1262 (hat?)";
                            in.gps_on = gps != 0; in.gps_port = gps != 1; in.gps_nmea = gps >= 3; in.gps_fix = gps == 5; in.gps_had_fix = gps == 4;
                            in.gps_on_ms = gps == 2 ? 100000 : 1000;
                            in.sats = (uint8_t)(c % 256); in.acc_dm = (uint16_t)(c % 65535);
                            in.sd_error = sd == 2; in.log_ok = sd == 0;
                            in.rows = c; in.pending = (uint32_t)(c % 33);
                            in.elapsed_s = c; in.sess_no = (int)(c % 10000);
                            in.last_age_ms = c;
                            in.toast = c == 7 ? "LoRa activado (solo RX)" : "";
                            for (uint8_t v = 0; v < 3; v++) {
                                Screen s = render(v, in);
                                check_layout(s, v == 0 ? "dashboard" : v == 1 ? "ayuda" : "diagnostico");
                                screens++;
                                if (v != WD_VIEW_DASH) continue;
                                check_no_banned(s, "dashboard");
                                CHECK(s.has("CAPTURA ") && s.has("SD: ") && s.exact("NO DISP"));
                                CHECK(s.exact(wd_ui_radio_txt(in.ble)) && s.exact(wd_ui_radio_txt(in.lora)));
                                CHECK(s.exact(wd_ui_gps_txt(wd_ui_gps(in))));
                                CHECK(s.has("ENTER") && s.has("H ayuda"));
                            }
                        }
    printf("%d pantallas revisadas\n", screens);
}

static void test_help_and_diag() {
    WdUiIn in = base();
    Screen h = render(WD_VIEW_HELP, in);
    CHECK(h.exact("CONTROLES"));
    // una acción por línea: tecla y descripción en la misma fila, cada tecla en una fila distinta
    const char *keys[] = {"ENTER", "B", "L", "G", "R", "S", "H"};
    const char *desc[] = {"iniciar / pausar", "BLE escuchar", "LoRa escuchar", "GPS on / off", "nueva sesion", "guardar y cerrar", "diagnostico / volver"};
    int last_y = -1;
    for (int k = 0; k < 7; k++) {
        const WdUiItem *a = h.exact(keys[k]), *b = h.exact(desc[k]);
        CHECK(a && b && a->y == b->y && a->y > last_y);
        if (a) last_y = a->y;
    }
    Screen d = render(WD_VIEW_DIAG, in);
    CHECK(d.has("DIAGNOSTICO") && d.has("v0.2.0"));
    CHECK(d.has("CAPTURA ACTIVA") && d.has("sesion 0007"));
    CHECK(d.has("BLE  ESCUCHANDO PASIVO") && d.has("24 ids 128 ev desc 0 ult 2s"));
    CHECK(d.has("WiFi NO DISPONIBLE") && d.has("ABI 1.8 sin monitor pasivo"));
    CHECK(d.has("LoRa RX 915.000MHz SF9 BW125 sync12") && d.has("7 ids 43 ev desc 0 ult 9s"));
    CHECK(d.has("GPS  FIX") && d.has("puerto abierto") && d.has("NMEA si") && d.has("8 sat  edad 0s  hdop 1.0"));
    CHECK(d.has("SD   GRABANDO session-0007.csv.part"));
    CHECK(d.has("H volver"));
    // el detalle técnico no está en el dashboard
    Screen s = render(WD_VIEW_DASH, in);
    CHECK(!s.has("MHz") && !s.has("SF9") && !s.has("sync") && !s.has("hdop"));
    // radio con error: el motivo en el diagnóstico
    in.lora.state = WD_RS_ERROR; in.lora.why = "no responde el SX1262 (hat?)";
    d = render(WD_VIEW_DIAG, in);
    const WdUiItem *w = d.find("no responde el SX1262");
    CHECK(d.has("LoRa ERROR") && w && w->fg == WD_C_RED);
}

static std::string read_file(const char *p) {
    std::string s;
    FILE *f = fopen(p, "rb");
    if (!f) return s;
    char b[512];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

// Lo que dicen los controles es lo que hacen las teclas en main.cpp, y no quedaron textos viejos
static void test_sources(const char *main_path, const char *ui_path) {
    std::string m = read_file(main_path), u = read_file(ui_path);
    CHECK(!m.empty() && !u.empty());
    const char *handled[] = {"case 'b':", "case 'l':", "case 'g':", "case 'r':", "case 's':", "case 'h':", "case '?':", "k.enter && !prev_enter"};
    for (const char *h : handled) { bool f = m.find(h) != std::string::npos; if (!f) printf("  falta en main.cpp: %s\n", h); CHECK(f); }
    const char *old[] = {"\"REC\"", "\"PAUSA\"", "\"on\"", "\"off\"", "R_TXT", "'*'", "\"unsupported\"\n"};
    for (const char *o : old) {
        bool f = m.find(o) != std::string::npos || u.find(o) != std::string::npos;
        if (f) printf("  texto viejo: %s\n", o);
        CHECK(!f);
    }
    CHECK(m.find("wd_ui_render(view") != std::string::npos); // main dibuja lo que arma wd_ui.h
    CHECK(m.find(".create(") == std::string::npos && m.find("destroy(") == std::string::npos); // sin framebuffer propio
}

int main(int argc, char **argv) {
    test_mockup();
    test_states();
    test_all_combinations();
    test_help_and_diag();
    test_sources(argc > 1 ? argv[1] : "apps/wardriving/src/main.cpp", argc > 2 ? argv[2] : "apps/wardriving/src/wd_ui.h");
    printf("%d checks, %d fails\n", checks, fails);
    return fails ? 1 : 0;
}
