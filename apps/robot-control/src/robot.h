// Interfaz común de los drivers de robots (cada familia implementa Robot en su drv_*.cpp).
#pragma once
#include <M5Cardputer.h>
#include <string>

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HUD_H = 15;

extern M5Canvas canvas;
extern uint16_t C_BG, C_HUD, C_TEXT, C_DIM, C_ACCENT, C_OK, C_WARN, C_ERR, C_VID;

void drawHud(const char *left, const char *right);

// lo que hay encontrado en el escaneo
enum Family : uint8_t { F_SUMO, F_SPHERO1, F_SPHERO2, F_MINI };
struct Found {
    Family fam;
    std::string name;
    uint8_t addr[6];  // BLE
    uint8_t type;
    int sub;          // Sphero v2: modelo
    int rssi;
};

// teclas de manejo mantenidas en este instante
struct Held {
    bool fwd, back, left, right;
    const std::string *word;
    bool has(char c) const { return word->find(c) != std::string::npos; }
};

struct Robot {
    bool dirty = true;
    bool fresh = true;  // el driver limpia toda la pantalla la primera vez
    virtual ~Robot() {}
    virtual bool connect(std::string &status) = 0;  // bloqueante; false deja `status` con el motivo
    virtual void tick(const Held &h) = 0;           // cada vuelta del loop: manejo y red
    virtual void key(char c) = 0;                   // tecla recién apretada
    virtual void enter() {}
    virtual void draw() = 0;                        // dibuja bajo el HUD (el HUD lo pone main)
    virtual const char *title() = 0;
    virtual const char *hint() { return "Fn+` salir"; }
    virtual bool linkOk() { return true; }
    virtual void close() = 0;                       // frena y se desconecta de forma limpia
};

Robot *make_sumo(const Found &f);
Robot *make_sphero1(const Found &f);
Robot *make_sphero2(const Found &f);
Robot *make_mini(const Found &f);

// ---- BLE compartido --------------------------------------------------------------------
// Tabla de características del robot conectado: se descubre una sola vez por conexión (cada
// consulta GATT cuesta idas y vueltas por el aire).
inline cpos_ble_attr_t g_chr[48];
inline int g_nchr = -1;

inline void loadChars()
{
    static cpos_ble_attr_t svc[16], ch[32];
    g_nchr = 0;
    int ns = cpos_api->ble.services(svc, 16);
    for (int s = 0; s < ns; s++) {
        int nc = cpos_api->ble.characteristics(s, ch, 32);
        for (int c = 0; c < nc && g_nchr < 48; c++) g_chr[g_nchr++] = ch[c];
    }
}

// Busca una característica con `match(uuid)`; devuelve su handle de valor, el final y las propiedades.
template <class F>
static bool findChr(F match, uint16_t &handle, uint16_t *end = nullptr, uint8_t *props = nullptr)
{
    if (g_nchr < 0) loadChars();
    for (int i = 0; i < g_nchr; i++)
        if (match(g_chr[i].uuid)) {
            handle = g_chr[i].handle;
            if (end) *end = g_chr[i].end_handle;
            if (props) *props = g_chr[i].props;
            return true;
        }
    return false;
}

inline bool bleConnect(const Found &f, std::string &status)
{
    cpos_api->ble.begin();
    if (cpos_api->ble.scanning()) cpos_api->ble.scan_stop();
    g_nchr = -1;
    if (cpos_api->ble.connect(f.addr, f.type, 10000) != 0) { status = "No pude conectar"; return false; }
    return true;
}

inline int bleWrite(uint16_t h, uint8_t props, const uint8_t *p, size_t n)
{
    return cpos_api->ble.write(h, p, n, (props & 0x04) ? 0 : 1);  // sin respuesta si la característica lo admite
}
