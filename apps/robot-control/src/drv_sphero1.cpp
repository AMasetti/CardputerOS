// Sphero v1: BB-8 original, Ollie. Teclas: ; . adelante/atrás, , / giran el rumbo, - = velocidad,
// 1-8 color, c calibrar (luz trasera + enter), s dormir, space frena.
#include "robot.h"
#include "sphero1_core.h"

namespace {

const uint8_t PALETTE[8][3] = {{255, 0, 0}, {255, 120, 0}, {255, 220, 0}, {0, 220, 60},
                               {0, 200, 220}, {0, 80, 255}, {170, 0, 255}, {255, 255, 255}};

struct Sphero1 : Robot {
    Found f;
    sphero1::Builder pb;
    uint16_t hCmd = 0;
    uint8_t props = 0;
    int speedPct = 50, heading = 0;
    bool calibrating = false, moving = false;
    uint8_t color[3] = {0, 120, 255};
    uint32_t lastRoll = 0;

    explicit Sphero1(const Found &x) : f(x) {}
    const char *title() override { return f.name.c_str(); }
    bool linkOk() override { return cpos_api->ble.connected(); }

    void write(const uint8_t *p, size_t n) { cpos_api->ble.write(hCmd, p, n, 1); }
    void roll(int speed, int hd)
    {
        uint8_t p[16];
        write(p, pb.roll(p, speed, hd));
    }
    void rgb(const uint8_t c[3])
    {
        uint8_t p[16];
        memcpy(color, c, 3);
        write(p, pb.rgb(p, c[0], c[1], c[2]));
    }

    bool connect(std::string &status) override
    {
        if (!bleConnect(f, status)) return false;
        uint16_t hDos, hTx, hWake;
        auto is = [](uint8_t chr) { return [chr](const uint8_t *u) { return sphero1::uuid_is(u, chr); }; };
        if (!findChr(is(sphero1::CHR_ANTIDOS), hDos) || !findChr(is(sphero1::CHR_TXPOWER), hTx) ||
            !findChr(is(sphero1::CHR_WAKE), hWake) || !findChr(is(sphero1::CHR_CMD), hCmd)) {
            cpos_api->ble.disconnect();
            status = "No es un Sphero v1";
            return false;
        }
        bool ok = cpos_api->ble.write(hDos, sphero1::WAKE_ANTIDOS, sizeof(sphero1::WAKE_ANTIDOS), 1) == 0 &&
                  cpos_api->ble.write(hTx, sphero1::WAKE_TXPOWER, sizeof(sphero1::WAKE_TXPOWER), 1) == 0 &&
                  cpos_api->ble.write(hWake, sphero1::WAKE_CPU, sizeof(sphero1::WAKE_CPU), 1) == 0;
        if (!ok) { cpos_api->ble.disconnect(); status = "El robot no despertó"; return false; }
        rgb(color);
        return true;
    }

    void tick(const Held &h) override
    {
        if (millis() - lastRoll < 100) return;
        lastRoll = millis();
        int turn = (h.right ? 1 : 0) - (h.left ? 1 : 0);
        if (turn) { heading = (heading + turn * 12 + 360) % 360; dirty = true; }
        int spd = 255 * speedPct / 100;
        bool active = !calibrating && (h.fwd || h.back);
        if (active != moving) { moving = active; dirty = true; }
        if (active) roll(spd, h.fwd ? heading : (heading + 180) % 360);
        else roll(0, heading);  // velocidad 0 frena y, al repetirse, mantiene vivo el enlace
    }

    void enter() override
    {
        if (!calibrating) return;
        uint8_t p[16];
        write(p, pb.set_heading(p, 0));
        write(p, pb.back_led(p, 0));
        heading = 0;
        calibrating = false;
        dirty = true;
    }

    void key(char c) override
    {
        uint8_t p[16];
        dirty = true;
        if (c == '-') speedPct = speedPct > 10 ? speedPct - 10 : 10;
        else if (c == '=' || c == '+') speedPct = speedPct < 100 ? speedPct + 10 : 100;
        else if (c >= '1' && c <= '8') rgb(PALETTE[c - '1']);
        else if (c == 'c') {
            calibrating = !calibrating;
            write(p, pb.back_led(p, calibrating ? 255 : 0));
        } else if (c == 's') write(p, pb.sleep(p));
        else if (c == ' ') roll(0, heading);
    }

    void draw() override
    {
        if (fresh) canvas.fillScreen(C_BG);
        canvas.fillRect(0, HUD_H, SCREEN_W, SCREEN_H - HUD_H, C_BG);
        const int cx = 60, cy = 75, R = 44;
        canvas.drawCircle(cx, cy, R, C_DIM);
        canvas.fillCircle(cx, cy, 10, canvas.color565(color[0], color[1], color[2]));
        float a = heading * 3.14159265f / 180.0f;
        int ex = cx + (int)(sinf(a) * (R - 4)), ey = cy - (int)(cosf(a) * (R - 4));
        canvas.drawLine(cx, cy, ex, ey, calibrating ? C_ERR : C_ACCENT);
        canvas.fillCircle(ex, ey, 3, calibrating ? C_ERR : C_ACCENT);
        canvas.setFont(&fonts::lgfxJapanGothicP_12);
        canvas.setTextDatum(top_left);
        char b[32];
        snprintf(b, sizeof(b), "Rumbo %d", heading);
        canvas.setTextColor(C_TEXT, C_BG);
        canvas.drawString(b, 130, HUD_H + 8);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("Vel", 130, HUD_H + 28);
        canvas.drawRoundRect(154, HUD_H + 30, 76, 8, 3, C_DIM);
        canvas.fillRoundRect(155, HUD_H + 31, 74 * speedPct / 100, 6, 2, C_ACCENT);
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(calibrating ? C_ERR : (moving ? C_OK : C_DIM), C_BG);
        canvas.drawString(calibrating ? "CALIBRANDO: enter ok" : (moving ? "en marcha" : "frenado"), 130, HUD_H + 48);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString(";. mover  ,/ girar", 130, HUD_H + 68);
        canvas.drawString("-= vel  1-8 color", 130, HUD_H + 80);
        canvas.drawString("c calibrar  s dormir", 130, HUD_H + 92);
        if (!linkOk()) {
            canvas.fillRoundRect(40, 50, 160, 30, 4, C_ERR);
            canvas.setTextColor(C_TEXT, C_ERR);
            canvas.setTextDatum(middle_center);
            canvas.drawString("SIN CONEXION", 120, 65);
        }
    }

    int intent(const Intent &in) override
    {
        if (in.is("robot.color")) {
            int r = in.i("r", -1), g = in.i("g", -1), b = in.i("b", -1);
            if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255) return CPOS_INTENT_BAD_ARG;
            const uint8_t c[3] = {(uint8_t)r, (uint8_t)g, (uint8_t)b};
            rgb(c);
            return CPOS_INTENT_OK;
        }
        if (in.is("robot.drive")) {
            int hd = intentHeading(in);
            int ms = in.i("ms", 1000), sp = in.i("speed", 50);
            if (hd < 0 || ms < 100 || ms > 5000 || sp < 10 || sp > 100) return CPOS_INTENT_BAD_ARG;
            for (uint32_t t0 = millis(); millis() - t0 < (uint32_t)ms; delay(100)) roll(255 * sp / 100, hd);
            roll(0, hd);
            return CPOS_INTENT_OK;
        }
        return CPOS_INTENT_NO_APP;
    }

    void close() override
    {
        if (cpos_api->ble.connected()) {
            roll(0, heading);
            delay(50);
            cpos_api->ble.disconnect();  // si no se corta bien, el robot rechaza la próxima conexión
        }
    }
};

}  // namespace

Robot *make_sphero1(const Found &f) { return new Sphero1(f); }
