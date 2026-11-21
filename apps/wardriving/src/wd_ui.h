// Interfaz de wardriving: qué se dibuja y dónde, sin tocar el OS.
//
// main.cpp junta el estado en un WdUiIn y dibuja lo que emiten wd_ui_dashboard / wd_ui_help / wd_ui_diag
// (texto, rectángulos y un punto) con la fuente fija 6x8 del OS, en el canvas de pantalla del OS. El test
// de host recorre las mismas funciones para revisar etiquetas, palabras prohibidas y que nada se salga
// de 240x135.
//
// Estados siempre en texto, con color redundante (convención de BLE Sentinel):
//   rojo = captura activa / error · verde = escuchando / fix · amarillo = pausada, buscando, sin fix, RAM
//   gris = apagado / no disponible · celeste = ayuda, última actividad, navegación
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WD_UI_W 240
#define WD_UI_H 135
#define WD_UI_CW 6   // ancho de carácter (FONT0 x1)
#define WD_UI_CH 8   // alto de carácter
#define WD_UI_COLS 39 // caracteres que entran con 2 px de margen a cada lado

constexpr uint16_t wd_rgb(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
enum WdColor : uint16_t {
    WD_C_BG = wd_rgb(10, 10, 14),
    WD_C_SEP = wd_rgb(50, 50, 58),
    WD_C_TEXT = wd_rgb(225, 225, 230),
    WD_C_GREY = wd_rgb(120, 120, 130),
    WD_C_RED = wd_rgb(221, 29, 33),
    WD_C_GREEN = wd_rgb(40, 200, 90),
    WD_C_YELLOW = wd_rgb(240, 200, 40),
    WD_C_CYAN = wd_rgb(80, 200, 230),
    WD_C_BLACK = 0x0000,
};

enum { WD_RS_OFF = 0, WD_RS_ON, WD_RS_ERROR };                         // estado real de una radio
enum { WD_CAP_DETENIDA = 0, WD_CAP_PAUSADA, WD_CAP_ACTIVA };
enum { WD_GPS_APAGADO = 0, WD_GPS_ERROR, WD_GPS_BUSCANDO, WD_GPS_SIN_FIX, WD_GPS_FIX };
enum { WD_SD_SIN_SESION = 0, WD_SD_GRABANDO, WD_SD_RAM, WD_SD_ERROR };
enum { WD_VIEW_DASH = 0, WD_VIEW_HELP, WD_VIEW_DIAG };
#define WD_GPS_NMEA_TIMEOUT_MS 8000 // puerto abierto y sin NMEA este tiempo: no hay receptor (ERROR)
#define WD_NO_AGE 0xFFFFFFFFu

struct WdUiRadio {
    uint8_t state = WD_RS_OFF;
    bool armed = false;
    uint32_t ids = 0, ev = 0, drop = 0;
    uint32_t last_age_ms = WD_NO_AGE; // última actividad
    const char *why = "";             // motivo de ERROR (diagnóstico)
};

struct WdUiIn {
    const char *version = "";
    bool running = false, sess_open = false;
    int sess_no = 0;
    uint32_t elapsed_s = 0;          // tiempo capturando en la sesión
    bool sd_error = false, log_ok = false;
    const char *sd_msg = "";         // motivo / último mensaje de la SD
    WdUiRadio ble, lora;
    uint32_t lora_freq_hz = 0, lora_bw_hz = 0;
    uint8_t lora_sf = 0, lora_sync = 0;
    // GPS
    bool gps_on = false, gps_port = false, gps_nmea = false, gps_fix = false, gps_had_fix = false;
    uint32_t gps_on_ms = 0;          // desde que se prendió
    uint8_t sats = 0;
    uint16_t acc_dm = 0xFFFF;
    uint16_t hdop10 = 0;
    uint32_t fix_age_ms = WD_NO_AGE;
    // actividad
    uint8_t last_src = 255;          // WD_SRC_* del último evento
    uint32_t last_age_ms = WD_NO_AGE;
    uint32_t rows = 0, pending = 0, ring = 32;
    const char *toast = "";          // aviso corto de la última tecla (reemplaza "Ultimo:" un momento)
};

enum { WD_UI_TEXT = 0, WD_UI_FILL, WD_UI_DOT, WD_UI_HLINE };
struct WdUiItem {
    uint8_t kind;
    int16_t x, y, w, h;
    uint16_t fg, bg;
    char text[WD_UI_COLS + 2];
};

// ---- estados ---------------------------------------------------------------------------------------
static uint8_t wd_ui_capture(const WdUiIn &in) { return in.running ? WD_CAP_ACTIVA : in.sess_open ? WD_CAP_PAUSADA : WD_CAP_DETENIDA; }
static const char *wd_ui_capture_txt(uint8_t c) { return c == WD_CAP_ACTIVA ? "ACTIVA" : c == WD_CAP_PAUSADA ? "PAUSADA" : "DETENIDA"; }

static const char *wd_ui_radio_txt(const WdUiRadio &r) { return r.state == WD_RS_ON ? "ESCUCHANDO" : r.state == WD_RS_ERROR ? "ERROR" : "APAGADO"; }
static uint16_t wd_ui_radio_color(const WdUiRadio &r) { return r.state == WD_RS_ON ? WD_C_GREEN : r.state == WD_RS_ERROR ? WD_C_RED : WD_C_GREY; }

// FIX solo con un fix fresco (WD_FIX_MAX_AGE_MS, ya aplicado en gps_fix); puerto abierto no es fix
static uint8_t wd_ui_gps(const WdUiIn &in) {
    if (!in.gps_on) return WD_GPS_APAGADO;
    if (!in.gps_port) return WD_GPS_ERROR;
    if (in.gps_fix) return WD_GPS_FIX;
    if (!in.gps_nmea) return in.gps_on_ms > WD_GPS_NMEA_TIMEOUT_MS ? WD_GPS_ERROR : WD_GPS_BUSCANDO;
    return in.gps_had_fix ? WD_GPS_SIN_FIX : WD_GPS_BUSCANDO;
}
static const char *wd_ui_gps_txt(uint8_t g) {
    static const char *const T[] = {"APAGADO", "ERROR", "BUSCANDO", "SIN FIX", "FIX"};
    return T[g < 5 ? g : 1];
}
static uint16_t wd_ui_gps_color(uint8_t g) { return g == WD_GPS_FIX ? WD_C_GREEN : g == WD_GPS_ERROR ? WD_C_RED : g == WD_GPS_APAGADO ? WD_C_GREY : WD_C_YELLOW; }

// GRABANDO = sesión abierta y el último lote escribió bien; RAM = la captura sigue pero no se guarda
static uint8_t wd_ui_sd(const WdUiIn &in) {
    if (in.sd_error) return WD_SD_ERROR;
    if (in.sess_open && in.log_ok) return WD_SD_GRABANDO;
    if (in.running) return WD_SD_RAM;
    return WD_SD_SIN_SESION;
}
static const char *wd_ui_sd_txt(uint8_t s) {
    static const char *const T[] = {"SIN SESION", "GRABANDO", "RAM", "ERROR"};
    return T[s < 4 ? s : 3];
}
static uint16_t wd_ui_sd_color(uint8_t s) { return s == WD_SD_ERROR ? WD_C_RED : s == WD_SD_GRABANDO ? WD_C_GREEN : s == WD_SD_RAM ? WD_C_YELLOW : WD_C_GREY; }

static const char *wd_ui_src_txt(uint8_t s) { return s == 0 ? "BLE" : s == 1 ? "WiFi" : s == 2 ? "LoRa" : "--"; }

// ---- texto acotado ---------------------------------------------------------------------------------
struct WdUiStr {
    char b[WD_UI_COLS + 2];
    size_t n = 0;
    WdUiStr() { b[0] = 0; }
    WdUiStr &s(const char *p) { while (*p && n < WD_UI_COLS) b[n++] = *p++; b[n] = 0; return *this; }
    WdUiStr &u(uint32_t v) { char t[11]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k && n < WD_UI_COLS) b[n++] = t[--k]; b[n] = 0; return *this; }
    WdUiStr &u2(uint32_t v) { if (v < 10) s("0"); return u(v); }
    WdUiStr &n4(int v) { uint32_t x = v < 0 ? 0 : (uint32_t)v; for (uint32_t d = 1000; d > 1 && x < d; d /= 10) s("0"); return u(x); }
    WdUiStr &pad(size_t col) { while (n < col && n < WD_UI_COLS) b[n++] = ' '; b[n] = 0; return *this; }
    // contador compacto: 9999, 12k, 3M
    WdUiStr &cnt(uint32_t v) { if (v < 10000) return u(v); if (v < 1000000) return u(v / 1000).s("k"); return u(v / 1000000).s("M"); }
    WdUiStr &age(uint32_t ms) {
        if (ms == WD_NO_AGE) return s("--");
        uint32_t t = ms / 1000;
        if (t < 60) return u(t).s("s");
        if (t < 3600) return u(t / 60).s("m");
        return u(t / 3600).s("h");
    }
    WdUiStr &hhmmss(uint32_t t) { return u2(t / 3600 % 100).s(":").u2(t / 60 % 60).s(":").u2(t % 60); }
};

template <class F> static void wd_ui_text(F &emit, int x, int y, uint16_t fg, const char *t, uint16_t bg = WD_C_BG) {
    WdUiItem it;
    it.kind = WD_UI_TEXT; it.x = (int16_t)x; it.y = (int16_t)y; it.fg = fg; it.bg = bg;
    size_t l = strlen(t);
    size_t maxc = (size_t)((WD_UI_W - 2 - x) / WD_UI_CW); // nunca se sale de la pantalla
    if (l > maxc) l = maxc;
    if (l > WD_UI_COLS) l = WD_UI_COLS;
    memcpy(it.text, t, l);
    it.text[l] = 0;
    it.w = (int16_t)(l * WD_UI_CW); it.h = WD_UI_CH;
    emit(it);
}
template <class F> static void wd_ui_right(F &emit, int y, uint16_t fg, const char *t, uint16_t bg = WD_C_BG) {
    wd_ui_text(emit, WD_UI_W - 2 - (int)strlen(t) * WD_UI_CW, y, fg, t, bg);
}
template <class F> static void wd_ui_rect(F &emit, uint8_t kind, int x, int y, int w, int h, uint16_t c) {
    WdUiItem it;
    it.kind = kind; it.x = (int16_t)x; it.y = (int16_t)y; it.w = (int16_t)w; it.h = (int16_t)h; it.fg = c; it.bg = c; it.text[0] = 0;
    emit(it);
}

// ---- dashboard ---------------------------------------------------------------------------------------
#define WD_UI_STATE_X (6 * WD_UI_CW + 2) // columna del estado de cada fila

// fila de radio: nombre, estado (siempre entero) y, si entra, el dato secundario a la derecha
template <class F> static void wd_ui_radio_row(F &emit, int y, const char *label, const WdUiRadio &r, bool running) {
    wd_ui_text(emit, 2, y, WD_C_TEXT, label);
    const char *st = wd_ui_radio_txt(r);
    wd_ui_text(emit, WD_UI_STATE_X, y, wd_ui_radio_color(r), st);
    int state_end = WD_UI_STATE_X + (int)strlen(st) * WD_UI_CW + WD_UI_CW;
    WdUiStr full, counts;
    counts.cnt(r.ids).s(" ids ").cnt(r.ev).s(" ev");
    if (!running && r.armed && r.state != WD_RS_ERROR) full.s("listo ");
    full.s(counts.b);
    const char *sec = WD_UI_W - 2 - (int)full.n * WD_UI_CW >= state_end ? full.b
                    : WD_UI_W - 2 - (int)counts.n * WD_UI_CW >= state_end ? counts.b : "";
    if (*sec) wd_ui_right(emit, y, r.state == WD_RS_ON ? WD_C_TEXT : WD_C_GREY, sec);
}

template <class F> static void wd_ui_dashboard(const WdUiIn &in, F emit) {
    wd_ui_rect(emit, WD_UI_FILL, 0, 0, WD_UI_W, WD_UI_H, WD_C_BG);
    // encabezado: CAPTURA + ACTIVA / PAUSADA / DETENIDA y el tiempo de captura de la sesión
    uint8_t cap = wd_ui_capture(in);
    uint16_t bar = cap == WD_CAP_ACTIVA ? WD_C_RED : cap == WD_CAP_PAUSADA ? WD_C_YELLOW : WD_C_SEP;
    uint16_t ink = cap == WD_CAP_PAUSADA ? WD_C_BLACK : WD_C_TEXT;
    wd_ui_rect(emit, WD_UI_FILL, 0, 0, WD_UI_W, 13, bar);
    wd_ui_rect(emit, WD_UI_DOT, 7, 6, 3, 3, ink);
    WdUiStr h;
    h.s("CAPTURA ").s(wd_ui_capture_txt(cap));
    wd_ui_text(emit, 14, 3, ink, h.b, bar);
    WdUiStr t;
    if (in.sess_open) t.hhmmss(in.elapsed_s); else t.s("--:--:--");
    wd_ui_right(emit, 3, ink, t.b, bar);

    // sesión y SD
    WdUiStr s;
    if (in.sess_open) s.s("Sesion ").n4(in.sess_no);
    else s.s("Sin sesion");
    wd_ui_text(emit, 2, 17, WD_C_TEXT, s.b);
    uint8_t sd = wd_ui_sd(in);
    WdUiStr sdt;
    sdt.s("SD: ").s(wd_ui_sd_txt(sd));
    wd_ui_right(emit, 17, wd_ui_sd_color(sd), sdt.b);
    wd_ui_rect(emit, WD_UI_HLINE, 0, 28, WD_UI_W, 1, WD_C_SEP);

    // radios y GPS
    wd_ui_radio_row(emit, 32, "BLE", in.ble, in.running);
    wd_ui_text(emit, 2, 43, WD_C_TEXT, "WiFi");
    wd_ui_text(emit, WD_UI_STATE_X, 43, WD_C_GREY, "NO DISP");
    wd_ui_right(emit, 43, WD_C_GREY, "ABI 1.8");
    wd_ui_radio_row(emit, 54, "LoRa", in.lora, in.running);
    uint8_t g = wd_ui_gps(in);
    wd_ui_text(emit, 2, 65, WD_C_TEXT, "GPS");
    wd_ui_text(emit, WD_UI_STATE_X, 65, wd_ui_gps_color(g), wd_ui_gps_txt(g));
    if (g == WD_GPS_FIX || g == WD_GPS_BUSCANDO || g == WD_GPS_SIN_FIX) {
        WdUiStr gs;
        gs.u(in.sats).s(" sat");
        if (g == WD_GPS_FIX && in.acc_dm != 0xFFFF) gs.s(" ~").u((in.acc_dm + 5) / 10).s("m");
        wd_ui_right(emit, 65, g == WD_GPS_FIX ? WD_C_TEXT : WD_C_GREY, gs.b);
    }
    wd_ui_rect(emit, WD_UI_HLINE, 0, 76, WD_UI_W, 1, WD_C_SEP);

    // última actividad (o el aviso de la última tecla) y muestras
    WdUiStr la;
    if (*in.toast) la.s(in.toast);
    else if (in.last_src > 2) la.s("Ultimo: sin eventos");
    else la.s("Ultimo: ").s(wd_ui_src_txt(in.last_src)).pad(16).s("hace ").age(in.last_age_ms);
    wd_ui_text(emit, 2, 80, WD_C_CYAN, la.b);
    WdUiStr mu;
    mu.s("Muestras: ").cnt(in.rows).pad(19).s("lote: ").u(in.pending).s("/").u(in.ring);
    wd_ui_text(emit, 2, 91, WD_C_TEXT, mu.b);
    wd_ui_rect(emit, WD_UI_HLINE, 0, 102, WD_UI_W, 1, WD_C_SEP);

    // controles, en dos líneas, iguales a lo que hacen las teclas
    WdUiStr c1;
    c1.s("ENTER ").s(in.running ? "pausa" : "iniciar").pad(14).s("B BLE  L LoRa  G GPS");
    wd_ui_text(emit, 2, 110, WD_C_CYAN, c1.b);
    wd_ui_text(emit, 2, 122, WD_C_CYAN, "R nueva       S guardar  H ayuda");
}

// ---- ayuda: una acción por línea ----------------------------------------------------------------------
static const char *const WD_UI_HELP[][2] = {
    {"ENTER", "iniciar / pausar"}, {"B", "BLE escuchar"}, {"L", "LoRa escuchar"}, {"G", "GPS on / off"},
    {"R", "nueva sesion"}, {"S", "guardar y cerrar"}, {"H", "diagnostico / volver"},
};

template <class F> static void wd_ui_help(const WdUiIn &in, F emit) {
    wd_ui_rect(emit, WD_UI_FILL, 0, 0, WD_UI_W, WD_UI_H, WD_C_BG);
    wd_ui_rect(emit, WD_UI_FILL, 0, 0, WD_UI_W, 13, WD_C_SEP);
    wd_ui_text(emit, 4, 3, WD_C_CYAN, "CONTROLES", WD_C_SEP);
    WdUiStr v;
    v.s("v").s(in.version);
    wd_ui_right(emit, 3, WD_C_GREY, v.b, WD_C_SEP);
    int y = 18;
    for (auto &l : WD_UI_HELP) {
        wd_ui_text(emit, 4, y, WD_C_CYAN, l[0]);
        wd_ui_text(emit, 4 + 7 * WD_UI_CW, y, WD_C_TEXT, l[1]);
        y += 12;
    }
    wd_ui_text(emit, 4, 124, WD_C_GREY, "Solo escucha: no transmite ni conecta");
}

// ---- diagnóstico: el detalle que no va en el dashboard ------------------------------------------------
template <class F> static void wd_ui_radio_diag(F &emit, int &y, const char *label, const WdUiRadio &r, const char *first) {
    WdUiStr a, b;
    a.s(label).pad(5);
    if (r.state == WD_RS_ON) a.s(first);
    else a.s(wd_ui_radio_txt(r));
    wd_ui_text(emit, 2, y, wd_ui_radio_color(r), a.b);
    y += 10;
    if (r.state == WD_RS_ERROR && *r.why) b.s("     ").s(r.why);
    else b.s("     ").cnt(r.ids).s(" ids ").cnt(r.ev).s(" ev desc ").cnt(r.drop).s(" ult ").age(r.last_age_ms);
    wd_ui_text(emit, 2, y, r.state == WD_RS_ERROR ? WD_C_RED : WD_C_CYAN, b.b);
    y += 10;
}

template <class F> static void wd_ui_diag(const WdUiIn &in, F emit) {
    wd_ui_rect(emit, WD_UI_FILL, 0, 0, WD_UI_W, WD_UI_H, WD_C_BG);
    wd_ui_rect(emit, WD_UI_FILL, 0, 0, WD_UI_W, 12, WD_C_SEP);
    WdUiStr t;
    t.s("DIAGNOSTICO  v").s(in.version);
    wd_ui_text(emit, 2, 2, WD_C_CYAN, t.b, WD_C_SEP);
    int y = 14;
    WdUiStr c;
    c.s("CAPTURA ").s(wd_ui_capture_txt(wd_ui_capture(in)));
    if (in.sess_open) c.s("  sesion ").n4(in.sess_no);
    wd_ui_text(emit, 2, y, WD_C_TEXT, c.b);
    y += 10;
    wd_ui_radio_diag(emit, y, "BLE", in.ble, "ESCUCHANDO PASIVO");
    wd_ui_text(emit, 2, y, WD_C_GREY, "WiFi NO DISPONIBLE");
    y += 10;
    wd_ui_text(emit, 2, y, WD_C_GREY, "     ABI 1.8 sin monitor pasivo");
    y += 10;
    WdUiStr lr;
    lr.s("RX ").u(in.lora_freq_hz / 1000000).s(".");
    uint32_t khz = in.lora_freq_hz / 1000 % 1000;
    if (khz < 100) lr.s("0");
    if (khz < 10) lr.s("0");
    lr.u(khz).s("MHz SF").u(in.lora_sf).s(" BW").u(in.lora_bw_hz / 1000).s(" sync");
    const char *hx = "0123456789ABCDEF";
    char sw[3] = {hx[in.lora_sync >> 4], hx[in.lora_sync & 15], 0};
    lr.s(sw);
    wd_ui_radio_diag(emit, y, "LoRa", in.lora, lr.b);
    uint8_t g = wd_ui_gps(in);
    WdUiStr g1, g2;
    g1.s("GPS  ").s(wd_ui_gps_txt(g)).s("  puerto ").s(in.gps_port ? "abierto" : "cerrado").s(" NMEA ").s(in.gps_nmea ? "si" : "no");
    wd_ui_text(emit, 2, y, wd_ui_gps_color(g), g1.b);
    y += 10;
    g2.s("     ").u(in.sats).s(" sat  edad ").age(in.gps_fix ? in.fix_age_ms : WD_NO_AGE);
    if (in.hdop10) g2.s("  hdop ").u(in.hdop10 / 10).s(".").u(in.hdop10 % 10);
    wd_ui_text(emit, 2, y, WD_C_CYAN, g2.b);
    y += 10;
    uint8_t sd = wd_ui_sd(in);
    WdUiStr s1, s2;
    s1.s("SD   ").s(wd_ui_sd_txt(sd));
    if (in.sess_open) s1.s(" session-").n4(in.sess_no).s(".csv.part");
    wd_ui_text(emit, 2, y, wd_ui_sd_color(sd), s1.b);
    y += 10;
    s2.s("     ").s(in.sd_msg);
    wd_ui_text(emit, 2, y, sd == WD_SD_ERROR ? WD_C_RED : WD_C_CYAN, s2.b);
    wd_ui_text(emit, 2, 125, WD_C_CYAN, "H volver");
}

template <class F> static void wd_ui_render(uint8_t view, const WdUiIn &in, F emit) {
    if (view == WD_VIEW_HELP) wd_ui_help(in, emit);
    else if (view == WD_VIEW_DIAG) wd_ui_diag(in, emit);
    else wd_ui_dashboard(in, emit);
}
