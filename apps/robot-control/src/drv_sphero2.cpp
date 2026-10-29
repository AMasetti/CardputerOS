// Sphero v2: R2-D2, R2-Q5, BB-9E, BOLT. Teclas: ; . adelante/atrás, , / giran el rumbo, - = velocidad,
// 1-8 color, c calibrar (girar y enter = ese es el frente), space frena, s dormir.
// R2-D2/R2-Q5: h g cabeza izquierda/derecha, w t y patas (waddle / dos / tres), p sonido.
#include "robot.h"
#include "sphero2_core.h"

namespace {

const uint8_t PALETTE[8][3] = {{255, 0, 0}, {255, 120, 0}, {255, 220, 0}, {0, 220, 60},
                               {0, 200, 220}, {0, 80, 255}, {170, 0, 255}, {255, 255, 255}};
const uint16_t SOUND_HEY = 2813;  // R2_HEY_1

struct Sphero2 : Robot {
    Found f;
    sphero2::Kind kind;
    sphero2::Builder pb;
    uint16_t hApi = 0;
    uint8_t props = 0;
    int speedPct = 50, heading = 0, headDeg = 0;
    bool calibrating = false, moving = false;
    uint8_t color[3] = {0, 120, 255};
    uint32_t lastDrive = 0;
    uint8_t driveTid() const { return kind == sphero2::BOLT ? sphero2::TID_SECONDARY : sphero2::TID_NONE; }
    uint8_t powerTid() const { return kind == sphero2::BOLT ? sphero2::TID_PRIMARY : sphero2::TID_NONE; }
    bool hasHead() const { return kind == sphero2::R2D2 || kind == sphero2::R2Q5; }

    explicit Sphero2(const Found &x) : f(x), kind((sphero2::Kind)x.sub) {}
    const char *title() override { return f.name.c_str(); }
    bool linkOk() override { return cpos_api->ble.connected(); }

    void write(const uint8_t *p, size_t n) { bleWrite(hApi, props, p, n); }
    void rgb(const uint8_t c[3])
    {
        uint8_t p[40], v[12];
        memcpy(color, c, 3);
        size_t nv = sphero2::color_vals(kind, v, c[0], c[1], c[2]);
        write(p, pb.leds(p, sphero2::color_mask(kind), v, nv, kind == sphero2::BOLT ? sphero2::TID_PRIMARY : 0));
    }
    void drive(int speed, int hd)
    {
        uint8_t p[40];
        write(p, pb.drive(p, speed, hd, 0, driveTid()));
    }

    bool connect(std::string &status) override
    {
        if (!bleConnect(f, status)) return false;
        uint16_t hDos, end;
        auto isApi = [](const uint8_t *u) { return !memcmp(u, sphero2::UUID_API, 16); };
        auto isDos = [](const uint8_t *u) { return !memcmp(u, sphero2::UUID_ANTIDOS, 16); };
        if (!findChr(isDos, hDos) || !findChr(isApi, hApi, &end, &props)) {
            cpos_api->ble.disconnect();
            status = "No es un Sphero v2";
            return false;
        }
        cpos_api->ble.subscribe(hApi, end, 1);
        if (cpos_api->ble.write(hDos, sphero2::ANTIDOS_KEY, sizeof(sphero2::ANTIDOS_KEY), 1) != 0) {
            cpos_api->ble.disconnect();
            status = "El robot rechazó la clave";
            return false;
        }
        uint8_t p[40];
        write(p, pb.wake(p, powerTid()));
        delay(800);
        write(p, pb.reset_yaw(p, driveTid()));
        rgb(color);
        return true;
    }

    void tick(const Held &h) override
    {
        uint8_t rx[64];
        uint16_t hd;
        while (cpos_api->ble.notify_read(&hd, rx, sizeof(rx)) >= 0) {}  // las respuestas no se usan: se vacía la cola
        if (millis() - lastDrive < 100) return;
        lastDrive = millis();
        int turn = (h.right ? 1 : 0) - (h.left ? 1 : 0);
        if (turn) { heading = (heading + turn * 12 + 360) % 360; dirty = true; }
        bool active = !calibrating && (h.fwd || h.back);
        if (active != moving) { moving = active; dirty = true; }
        int spd = 255 * speedPct / 100;
        if (active) drive(spd, h.fwd ? heading : (heading + 180) % 360);
        else drive(0, heading);
    }

    void enter() override
    {
        if (!calibrating) return;
        uint8_t p[40];
        write(p, pb.reset_yaw(p, driveTid()));
        heading = 0;
        calibrating = false;
        dirty = true;
    }

    void key(char c) override
    {
        uint8_t p[40];
        dirty = true;
        if (c == '-') speedPct = speedPct > 10 ? speedPct - 10 : 10;
        else if (c == '=' || c == '+') speedPct = speedPct < 100 ? speedPct + 10 : 100;
        else if (c >= '1' && c <= '8') rgb(PALETTE[c - '1']);
        else if (c == 'c') calibrating = !calibrating;
        else if (c == ' ') drive(0, heading);
        else if (c == 's') write(p, pb.sleep(p, powerTid()));
        else if (hasHead() && (c == 'h' || c == 'g')) {
            headDeg += c == 'h' ? -30 : 30;
            if (headDeg < -160) headDeg = -160;
            if (headDeg > 180) headDeg = 180;
            write(p, pb.head(p, (float)headDeg));
        } else if (hasHead() && c == 'w') write(p, pb.legs(p, sphero2::LEG_WADDLE));
        else if (hasHead() && c == 't') write(p, pb.legs(p, sphero2::LEG_TWO));
        else if (hasHead() && c == 'y') write(p, pb.legs(p, sphero2::LEG_THREE));
        else if (c == 'p') write(p, pb.sound(p, SOUND_HEY));
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
        snprintf(b, sizeof(b), "%s  %d", sphero2::kind_name(kind), heading);
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
        canvas.drawString(";. mover  ,/ girar", 130, HUD_H + 64);
        canvas.drawString("-= vel  1-8 color", 130, HUD_H + 75);
        canvas.drawString("c calibrar  s dormir", 130, HUD_H + 86);
        if (hasHead()) canvas.drawString("hg cabeza wty patas", 130, HUD_H + 97);
        canvas.drawString("p sonido", 130, HUD_H + 108);
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
            for (uint32_t t0 = millis(); millis() - t0 < (uint32_t)ms; delay(100)) drive(255 * sp / 100, hd);
            drive(0, hd);
            return CPOS_INTENT_OK;
        }
        if (in.is("robot.sound")) {
            if (!hasHead()) return CPOS_INTENT_BAD_ARG;  // solo los R2
            uint8_t p[40];
            write(p, pb.sound(p, SOUND_HEY));
            delay(300);
            return CPOS_INTENT_OK;
        }
        return CPOS_INTENT_NO_APP;
    }

    void close() override
    {
        if (cpos_api->ble.connected()) {
            drive(0, heading);
            delay(50);
            cpos_api->ble.disconnect();
        }
    }
};

}  // namespace

Robot *make_sphero2(const Found &f) { return new Sphero2(f); }
