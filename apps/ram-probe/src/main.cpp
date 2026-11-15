// ram-probe: app mínima para aislar la carga de apps. Sin permisos, sin librerías,
// dibuja directo en la pantalla (sin canvas propio) el heap libre que ve la app.
// Salir: Fn + ` (lo maneja el OS; loop() vuelve enseguida para que el flag se atienda).

#include <M5Cardputer.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static uint32_t lastKb = 0xFFFFFFFFu;

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
}

void loop()
{
    uint32_t kb = (uint32_t)(cpos_api->free_heap() / 1024);
    if (kb != lastKb) {
        lastKb = kb;
        auto &d = M5Cardputer.Display;
        d.fillScreen(0x000000u);
        d.setTextColor(0xFFFFFFu);
        d.setTextSize(2);
        d.drawString("ram-probe " APP_VERSION, 4, 4);
        d.setTextSize(1);
        d.drawString("cargada: OK", 4, 34);
        d.drawString("free heap (KB):", 4, 50);
        d.setTextSize(3);
        d.drawNumber((long)kb, 4, 64);
        d.setTextSize(1);
        d.drawString("salir: Fn + `", 4, 118);
    }
    delay(200);
}
