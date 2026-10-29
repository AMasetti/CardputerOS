// Parrot Jumping Sumo por WiFi, con video. El Sumo crea su red (JumpingSumo_xxxxxx, sin clave): se
// une, hace el handshake por TCP (192.168.2.1:44444) y habla por UDP (manejo a 40 Hz, video MJPEG).
// Teclas: ; . , /  mover (mantenidas)   - =  velocidad   j l  salto alto / largo
//   1..9 animaciones (giro, golpecito, temblor, metrónomo, ondulación, giro+salto, giro a postura,
//   espiral, slalom)   u k y  postura saltador / pateador / parado   space  frena   v  video on/off
#include "robot.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <string>

#include "sumo_core.h"

namespace {

constexpr int VID_W = 160;
constexpr int VID_H = 120;
constexpr uint32_t PCMD_MS = 25;
constexpr uint32_t LINK_TIMEOUT_MS = 3000;
const uint32_t SUMO_IP = 0x0102A8C0;  // 192.168.2.1 en orden de red (lwip)
constexpr int DISCOVERY_PORT = 44444;
constexpr int D2C_PORT = 43210;

// JPEG desde memoria para el decodificador del OS (cpos_api->media.jpeg_draw)
struct MemReader { const uint8_t *d; size_t n, pos; };
size_t memRead(void *ctx, uint8_t *buf, size_t n)
{
    auto *m = static_cast<MemReader *>(ctx);
    size_t left = m->n - m->pos;
    if (n > left) n = left;
    if (buf) memcpy(buf, m->d + m->pos, n);  // buf nulo = saltear n bytes
    m->pos += n;
    return n;
}

struct Sumo : Robot, sumo::Listener {
    std::string ssid;
    sumo::Link link;
    int udp = -1;
    cpos_sockaddr_in drone{};
    uint32_t lastRx = 0, lastPcmd = 0, lastDecode = 0;
    bool videoOn = true, haveVideo = false, moving = false;
    int battery = -1, posture = 0, alert = 0, speedPct = 50;

    explicit Sumo(const Found &f) : ssid(f.name), link(this) {}
    const char *title() override { return ssid.c_str(); }
    bool linkOk() override { return millis() - lastRx <= LINK_TIMEOUT_MS; }

    void send(const uint8_t *d, size_t n) override
    {
        if (udp >= 0) cpos_api->net.sendto(udp, d, n, 0, &drone, sizeof(drone));
    }
    void on_jpeg(const uint8_t *d, size_t n) override
    {
        // la decodificación tarda: si venimos apurados se saltea el cuadro para no demorar el manejo
        if (millis() - lastDecode < 70) return;
        lastDecode = millis();
        MemReader r{d, n, 0};
        if (cpos_api->media.jpeg_draw(canvas.target(), 0, HUD_H, VID_W, VID_H, memRead, &r) == 0) {  // 640x480 -> 1/4
            haveVideo = true;
            dirty = true;
        }
    }
    void on_battery(int p) override { battery = p; dirty = true; }
    void on_posture(int p) override { posture = p; dirty = true; }
    void on_alert(int a) override { alert = a; dirty = true; }
    void on_jump_load(int) override {}

    void closeSockets()
    {
        if (udp >= 0) cpos_api->net.close(udp);
        udp = -1;
    }

    bool connect(std::string &status) override
    {
        WiFi.disconnect();
        WiFi.begin(ssid.c_str(), "");
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED) {
            M5Cardputer.update();
            if (millis() - t0 > 15000) { status = "No pude unirme al WiFi del robot"; return false; }
            delay(100);
        }
        // handshake por TCP
        int tcp = cpos_api->net.socket(2, 1, 0);
        if (tcp < 0) { status = "Sin sockets disponibles"; return false; }
        cpos_sockaddr_in a = {sizeof(a), 2, (uint16_t)(((DISCOVERY_PORT & 0xFF) << 8) | (DISCOVERY_PORT >> 8)), SUMO_IP, {0}};
        if (cpos_api->net.sock_connect(tcp, &a, sizeof(a)) != 0) {
            cpos_api->net.close(tcp);
            status = "El robot no responde (puerto 44444)";
            return false;
        }
        char hello[128];
        snprintf(hello, sizeof(hello), "{\"controller_type\":\"cardputer\",\"controller_name\":\"cardputeros\",\"d2c_port\":\"%d\"}", D2C_PORT);
        cpos_api->net.sock_send(tcp, hello, strlen(hello) + 1, 0);
        cpos_api->net.fcntl(tcp, 4, 1);
        char reply[512] = {0};
        int got = -1;
        for (t0 = millis(); millis() - t0 < 3000 && got <= 0;) {
            got = cpos_api->net.sock_recv(tcp, reply, sizeof(reply) - 1, 0);
            if (got <= 0) delay(20);
        }
        cpos_api->net.close(tcp);
        if (got <= 0) { status = "Sin respuesta del robot"; return false; }
        int c2d = sumo::Link::parse_c2d_port(reply, 54321);

        closeSockets();
        udp = cpos_api->net.socket(2, 2, 0);
        if (udp < 0) { status = "Sin sockets UDP"; return false; }
        int one = 1;
        cpos_api->net.setsockopt(udp, 0xfff, 0x0004, &one, sizeof(one));
        cpos_sockaddr_in me = {sizeof(me), 2, (uint16_t)(((D2C_PORT & 0xFF) << 8) | (D2C_PORT >> 8)), 0, {0}};
        if (cpos_api->net.bind(udp, &me, sizeof(me)) != 0) { closeSockets(); status = "No pude abrir el puerto de video"; return false; }
        cpos_api->net.fcntl(udp, 4, 1);
        drone = {sizeof(drone), 2, (uint16_t)(((c2d & 0xFF) << 8) | (c2d >> 8)), SUMO_IP, {0}};
        lastRx = millis();
        link.send_all_states();
        link.send_video_enable(videoOn);
        return true;
    }

    void tick(const Held &h) override
    {
        static uint8_t buf[1536];
        for (int i = 0; i < 12; i++) {
            int n = cpos_api->net.recvfrom(udp, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0) break;
            lastRx = millis();
            link.on_datagram(buf, n);
        }
        int speed = (h.fwd ? speedPct : 0) - (h.back ? speedPct : 0);
        int turn = (h.right ? 1 : 0) - (h.left ? 1 : 0);
        int turnVal = turn * (speedPct < 30 ? 30 : speedPct);
        bool active = speed != 0 || turn != 0;
        if (active != moving) { moving = active; dirty = true; }
        if (millis() - lastPcmd >= PCMD_MS) {
            lastPcmd = millis();
            link.send_pcmd(active, speed, turnVal);  // sin teclas: flag 0 = el robot frena
        }
    }

    void key(char c) override
    {
        switch (c) {
            case '-': speedPct = speedPct > 10 ? speedPct - 10 : 10; break;
            case '=': case '+': speedPct = speedPct < 100 ? speedPct + 10 : 100; break;
            case 'j': link.send_jump(sumo::JUMP_HIGH); break;
            case 'l': link.send_jump(sumo::JUMP_LONG); break;
            case '1': link.send_animation(sumo::ANIM_SPIN); break;
            case '2': link.send_animation(sumo::ANIM_TAP); break;
            case '3': link.send_animation(sumo::ANIM_SLOWSHAKE); break;
            case '4': link.send_animation(sumo::ANIM_METRONOME); break;
            case '5': link.send_animation(sumo::ANIM_ONDULATION); break;
            case '6': link.send_animation(sumo::ANIM_SPINJUMP); break;
            case '7': link.send_animation(sumo::ANIM_SPINTOPOSTURE); break;
            case '8': link.send_animation(sumo::ANIM_SPIRAL); break;
            case '9': link.send_animation(sumo::ANIM_SLALOM); break;
            case 'u': link.send_posture(sumo::POSTURE_JUMPER); break;
            case 'k': link.send_posture(sumo::POSTURE_KICKER); break;
            case 'y': link.send_posture(sumo::POSTURE_STANDING); break;
            case ' ': link.send_animation(sumo::ANIM_STOP); break;
            case 'v':
                videoOn = !videoOn;
                haveVideo = false;
                link.send_video_enable(videoOn);
                break;
        }
        dirty = true;
    }

    static const char *postureName(int p)
    {
        switch (p) {
            case 0: return "parado";
            case 1: return "saltador";
            case 2: return "pateador";
            case 3: return "trabado";
        }
        return "?";
    }

    void draw() override
    {
        if (fresh) canvas.fillScreen(C_BG);
        // video (se dibuja solo cuando llega un cuadro); marco y placeholder
        if (!haveVideo) {
            canvas.fillRect(0, HUD_H, VID_W, VID_H, C_VID);
            canvas.setFont(&fonts::Font0);
            canvas.setTextColor(C_DIM, C_VID);
            canvas.setTextDatum(middle_center);
            canvas.drawString(videoOn ? "esperando video..." : "video apagado", VID_W / 2, HUD_H + VID_H / 2);
        }
        const int px = VID_W + 4, pw = SCREEN_W - px;
        canvas.fillRect(px, HUD_H, pw, VID_H, C_BG);
        canvas.setFont(&fonts::lgfxJapanGothicP_12);
        canvas.setTextDatum(top_left);
        char b[32];
        if (battery >= 0) snprintf(b, sizeof(b), "Bat %d%%", battery); else snprintf(b, sizeof(b), "Bat --");
        canvas.setTextColor(battery < 0 ? C_DIM : (battery < 20 ? C_ERR : (battery < 40 ? C_WARN : C_OK)), C_BG);
        canvas.drawString(b, px, HUD_H + 3);
        canvas.setTextColor(alert == 1 ? C_ERR : C_TEXT, C_BG);
        canvas.drawString(alert == 1 ? "BAT CRIT" : postureName(posture), px, HUD_H + 18);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString("Vel", px, HUD_H + 36);
        canvas.drawRoundRect(px + 24, HUD_H + 38, pw - 28, 8, 3, C_DIM);
        canvas.fillRoundRect(px + 25, HUD_H + 39, (pw - 30) * speedPct / 100, 6, 2, C_ACCENT);
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(moving ? C_OK : C_DIM, C_BG);
        canvas.drawString(moving ? "en marcha" : "frenado", px, HUD_H + 52);
        canvas.setTextColor(C_DIM, C_BG);
        canvas.drawString(";.,/ mover", px, HUD_H + 70);
        canvas.drawString("j l  saltos", px, HUD_H + 80);
        canvas.drawString("1-9 anim", px, HUD_H + 90);
        canvas.drawString("ukY postura", px, HUD_H + 100);
        canvas.drawString("-= vel v vid", px, HUD_H + 110);
        if (!linkOk()) {
            canvas.fillRoundRect(10, 50, 140, 30, 4, C_ERR);
            canvas.setTextColor(C_TEXT, C_ERR);
            canvas.setTextDatum(middle_center);
            canvas.drawString("SIN SEÑAL DEL ROBOT", 80, 65);
        }
    }

    void close() override
    {
        if (udp >= 0) link.send_pcmd(false, 0, 0);
        closeSockets();
        WiFi.disconnect();
    }
};

}  // namespace

Robot *make_sumo(const Found &f) { return new Sumo(f); }
