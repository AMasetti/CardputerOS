// Ekoparty 22 badge para M5Stack Cardputer / Cardputer-Adv
//
// Muestra el logo de la Ekoparty 22 y un QR a AMasetti.com.
// Cualquier tecla alterna entre la vista logo + QR y el QR a pantalla
// completa (más fácil de escanear desde lejos).
//
// El logo vive en src/logo.h (RGB565). Para regenerarlo:
//   python3 tools/img2h.py assets/ekoparty22.jpg src/logo.h --size 130

#include <M5Cardputer.h>

#include "logo.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static const char *QR_URL = "https://amasetti.com";
static const char *QR_LABEL = "AMasetti.com";

static bool bigQr = false;

static void drawQr(int x, int y, int size)
{
    // M5GFX pinta el cuadrado blanco y centra el código: con 25 módulos
    // (versión 2) quedan 4 px por módulo en 104 px y 5 px en 135 px.
    M5Cardputer.Display.qrcode(QR_URL, x, y, size, 2);
}

static void drawBadge()
{
    auto &d = M5Cardputer.Display;
    const uint16_t pink = d.color565(236, 64, 140);
    const uint16_t cyan = d.color565(64, 200, 220);

    d.fillScreen(TFT_BLACK);
    // logo.h guarda RGB565 nativo; con un uint16_t* a secas M5GFX asume
    // bytes invertidos (swap565_t), así que se indica el tipo explícito.
    d.pushImage(2, (d.height() - LOGO_H) / 2, LOGO_W, LOGO_H,
                reinterpret_cast<const lgfx::rgb565_t *>(LOGO));

    // QR a la derecha del logo (área de ~105 px de ancho).
    const int qrSize = 104;
    const int qrX = d.width() - qrSize - 2;
    drawQr(qrX, 2, qrSize);

    d.setTextDatum(top_center);
    d.setFont(&fonts::Font2);
    d.setTextColor(pink);
    d.drawString(QR_LABEL, qrX + qrSize / 2, qrSize + 6);
    d.setFont(&fonts::Font0);
    d.setTextColor(cyan);
    d.drawString("v" APP_VERSION, qrX + qrSize / 2, qrSize + 23);
}

static void drawBigQr()
{
    auto &d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);
    const int size = d.height();
    drawQr((d.width() - size) / 2, 0, size);
}

static void redraw()
{
    if (bigQr) {
        drawBigQr();
    } else {
        drawBadge();
    }
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(200);
    redraw();
}

void loop()
{
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        bigQr = !bigQr;
        redraw();
    }
    delay(10);
}
