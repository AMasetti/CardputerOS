// Parrot Rolling Spider (minidron por BLE). Teclas:
//   t despegar (hace el flat trim antes)   l aterrizar   x emergencia (corta los motores)
//   ; .  adelante / atrás      , /  girar      w s  subir / bajar      a d  lateral izq / der
//   1 2 3 4  flip adelante / atrás / derecha / izquierda      v ruedas      - =  intensidad
// Soltar las teclas lo deja flotando. Es un dron de interior: espacio libre y sin gente cerca.
#include "robot.h"
#include "minidrone_core.h"

namespace {

struct Mini : Robot {
    Found f;
    mini::Builder pb;
    uint16_t hPcmd = 0, hCmd = 0, hEmerg = 0, hState = 0, hBatt = 0;
    int power = 50;
    int battery = -1, state = -1;
    bool wheels = false;
    uint32_t lastPcmd = 0, lastDraw = 0;

    explicit Mini(const Found &x) : f(x) {}
    const char *title() override { return f.name.c_str(); }
    bool linkOk() override { return cpos_api->ble.connected(); }
    bool flying() const { return state == mini::HOVERING || state == mini::FLYING || state == mini::TAKING_OFF; }

    void cmd(const uint8_t *p, size_t n) { cpos_api->ble.write(hCmd, p, n, 1); }

    bool connect(std::string &status) override
    {
        if (!bleConnect(f, status)) return false;
        auto is = [](uint16_t id) { return [id](const uint8_t *u) { return mini::uuid_is(u, id); }; };
        if (!findChr(is(mini::CHR_PCMD), hPcmd) || !findChr(is(mini::CHR_CMD), hCmd) ||
            !findChr(is(mini::CHR_EMERGENCY), hEmerg)) {
            cpos_api->ble.disconnect();
            status = "No es un minidron Parrot";
            return false;
        }
        for (uint16_t id : mini::NOTIFY_IDS) {
            uint16_t h, end;
            if (!findChr(is(id), h, &end)) continue;
            if (id == mini::CHR_STATE) hState = h;
            if (id == mini::CHR_BATTERY) hBatt = h;
            cpos_api->ble.subscribe(h, end, 1);
        }
        uint8_t p[32];
        cmd(p, pb.handshake(p));
        delay(200);
        return true;
    }

    void tick(const Held &h) override
    {
        uint8_t rx[32];
        uint16_t hd;
        int n;
        while ((n = cpos_api->ble.notify_read(&hd, rx, sizeof(rx))) >= 0) {
            if (hd == hState) { int s = mini::parse_state(rx, n); if (s >= 0 && s != state) { state = s; dirty = true; } }
            else if (hd == hBatt) { int b = mini::parse_battery(rx, n); if (b != battery) { battery = b; dirty = true; } }
        }
        if (millis() - lastPcmd >= 50) {
            lastPcmd = millis();
            int pitch = (h.fwd ? power : 0) - (h.back ? power : 0);
            int yaw = (h.right ? power : 0) - (h.left ? power : 0);
            int gaz = (h.has('w') ? power : 0) - (h.has('s') ? power : 0);
            int roll = (h.has('d') ? power : 0) - (h.has('a') ? power : 0);
            bool moving = pitch || yaw || gaz || roll;
            uint8_t p[24];
            size_t len = pb.pcmd(p, moving, roll, pitch, yaw, gaz);
            cpos_api->ble.write(hPcmd, p, len, 0);
        }
        if (millis() - lastDraw > 1000) { lastDraw = millis(); dirty = true; }
    }

    void key(char c) override
    {
        uint8_t p[24];
        dirty = true;
        if (c == '-') power = power > 10 ? power - 10 : 10;
        else if (c == '=' || c == '+') power = power < 100 ? power + 10 : 100;
        else if (c == 't' && !flying()) { cmd(p, pb.flat_trim(p)); delay(100); cmd(p, pb.takeoff(p)); state = mini::TAKING_OFF; }
        else if (c == 'l') { cmd(p, pb.land(p)); }
        else if (c == 'x') { size_t n = pb.emergency(p); cpos_api->ble.write(hEmerg, p, n, 1); }
        else if (c == 'v') { wheels = !wheels; cmd(p, pb.wheels(p, wheels)); }
        else if (flying() && c >= '1' && c <= '4') cmd(p, pb.flip(p, (mini::Flip)(c - '1')));  // flips solo en vuelo
    }

    const char *stateName() const
    {
        switch (state) {
            case mini::LANDED: return "en tierra";
            case mini::TAKING_OFF: return "despegando";
            case mini::HOVERING: return "flotando";
            case mini::FLYING: return "volando";
            case mini::LANDING: return "aterrizando";
            case mini::EMERGENCY: return "EMERGENCIA";
        }
        return "?";
    }

    void draw() override
    {
        if (fresh) canvas.fillScreen(C_BG);
        canvas.fillRect(0, HUD_H, SCREEN_W, SCREEN_H - HUD_H, C_BG);
        canvas.setFont(&fonts::lgfxJapanGothicP_12);
        canvas.setTextDatum(top_left);
        char b[32];
        if (battery >= 0) snprintf(b, sizeof(b), "Bat %d%%", battery); else snprintf(b, sizeof(b), "Bat --");
        canvas.setTextColor(battery < 0 ? C_DIM : (battery < 20 ? C_ERR : (battery < 40 ? C_WARN : C_OK)), C_BG);
        canvas.drawString(b, 10, HUD_H + 8);
        canvas.setTextColor(state == mini::EMERGENCY ? C_ERR : C_TEXT, C_BG);
        canvas.drawString(state < 0 ? "estado ?" : stateName(), 10, HUD_H + 28);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("Int", 10, HUD_H + 48);
        canvas.drawRoundRect(36, HUD_H + 50, 90, 8, 3, C_DIM);
        canvas.fillRoundRect(37, HUD_H + 51, 88 * power / 100, 6, 2, C_ACCENT);
        canvas.setFont(&fonts::Font0);
        canvas.drawString(wheels ? "ruedas puestas" : "sin ruedas", 10, HUD_H + 70);
        canvas.drawString("t despegar  l aterrizar", 130, HUD_H + 8);
        canvas.drawString("x EMERGENCIA", 130, HUD_H + 20);
        canvas.drawString(";. adel/atras ,/ giro", 130, HUD_H + 36);
        canvas.drawString("w s alt   a d lateral", 130, HUD_H + 48);
        canvas.drawString("1-4 flips  v ruedas", 130, HUD_H + 60);
        canvas.drawString("-= intensidad", 130, HUD_H + 72);
        if (!linkOk()) {
            canvas.fillRoundRect(40, 90, 160, 26, 4, C_ERR);
            canvas.setTextColor(C_TEXT, C_ERR);
            canvas.setTextDatum(middle_center);
            canvas.drawString("SIN CONEXION", 120, 103);
        }
    }

    void close() override
    {
        if (!cpos_api->ble.connected()) return;
        uint8_t p[24];
        cmd(p, pb.land(p));  // si cerramos en el aire, aterriza solo
        delay(100);
        cpos_api->ble.disconnect();
    }
};

}  // namespace

Robot *make_mini(const Found &f) { return new Mini(f); }
