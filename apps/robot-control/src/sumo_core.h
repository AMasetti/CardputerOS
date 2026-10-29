// Parrot Jumping Sumo: protocol logic only. No sockets, no OS calls: the app and the host test
// feed it datagrams and it answers through Listener::send(). Written from the public ARSDK
// descriptions (libARNetworkAL frames, libARStream video fragments, ARCommands ids).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace sumo {

// network frame types and buffer ids
enum : uint8_t { FT_ACK = 1, FT_DATA = 2, FT_LOWLAT = 3, FT_DATA_ACK = 4 };
enum : uint8_t {
    ID_PING = 0, ID_PONG = 1,
    ID_C2D_NONACK = 10, ID_C2D_ACK = 11, ID_C2D_EMERGENCY = 12, ID_C2D_VIDEOACK = 13,
    ID_D2C_VIDEO = 125, ID_D2C_EVENT = 126, ID_D2C_NAV = 127,
};
static const int ACK_ID_OFFSET = 128;  // the ack of buffer N travels on buffer N + 128
static const int HDR = 7;              // type, id, seq, size(u32 LE, whole frame)

// commands: project, class, command
enum : uint8_t { PRJ_COMMON = 0, PRJ_SUMO = 3 };
enum : uint8_t { CL_COMMON = 4, CL_COMMONSTATE = 5 };
enum : uint8_t { CL_PILOTING = 0, CL_PILOTINGSTATE = 1, CL_ANIMATIONS = 2, CL_ANIMATIONSSTATE = 3, CL_MEDIASTREAMING = 18 };
enum Posture : uint32_t { POSTURE_STANDING = 0, POSTURE_JUMPER = 1, POSTURE_KICKER = 2 };
enum Jump : uint32_t { JUMP_LONG = 0, JUMP_HIGH = 1 };
enum Anim : uint32_t { ANIM_STOP = 0, ANIM_SPIN = 1, ANIM_TAP = 2, ANIM_SLOWSHAKE = 3, ANIM_METRONOME = 4,
                       ANIM_ONDULATION = 5, ANIM_SPINJUMP = 6, ANIM_SPINTOPOSTURE = 7, ANIM_SPIRAL = 8, ANIM_SLALOM = 9 };

static const int MAX_FRAGMENTS = 128;        // the ack mask has room for 128 fragments
static const size_t MAX_JPEG = 24 * 1024;    // a 640x480 frame from the Sumo camera is well below this

struct Listener {
    virtual ~Listener() {}
    virtual void send(const uint8_t *data, size_t len) = 0;     // UDP datagram to the drone
    virtual void on_jpeg(const uint8_t *data, size_t len) = 0;  // one complete video frame
    virtual void on_battery(int percent) {}
    virtual void on_posture(int posture) {}                     // 0 standing, 1 jumper, 2 kicker, 3 stuck
    virtual void on_alert(int alert) {}                         // 0 none, 1 critical, 2 low battery
    virtual void on_jump_load(int state) {}
};

class Link {
public:
    explicit Link(Listener *l) : L(l) { memset(seq_, 0, sizeof(seq_)); reset_video(); }

    // ---- frames we send --------------------------------------------------------------
    size_t frame(uint8_t *out, uint8_t type, uint8_t id, const uint8_t *payload, size_t n) {
        out[0] = type;
        out[1] = id;
        out[2] = ++seq_[id];
        uint32_t size = HDR + (uint32_t)n;
        out[3] = size; out[4] = size >> 8; out[5] = size >> 16; out[6] = size >> 24;
        if (n) memcpy(out + HDR, payload, n);
        return size;
    }
    // heartbeat: flag 1 = the speed/turn values are active, 0 = hover/stop
    void send_pcmd(bool active, int speed, int turn) {
        uint8_t p[7] = {PRJ_SUMO, CL_PILOTING, 0, 0, (uint8_t)(active ? 1 : 0), (uint8_t)(int8_t)clamp(speed), (uint8_t)(int8_t)clamp(turn)};
        out(FT_DATA, ID_C2D_NONACK, p, sizeof(p));
    }
    void send_all_states() { uint8_t p[4] = {PRJ_COMMON, CL_COMMON, 0, 0}; out(FT_DATA_ACK, ID_C2D_ACK, p, 4); }
    void send_video_enable(bool on) { uint8_t p[5] = {PRJ_SUMO, CL_MEDIASTREAMING, 0, 0, (uint8_t)(on ? 1 : 0)}; out(FT_DATA_ACK, ID_C2D_ACK, p, 5); }
    void send_jump(Jump j) { send_u32(CL_ANIMATIONS, 3, (uint32_t)j); }
    void send_animation(Anim a) { send_u32(CL_ANIMATIONS, 4, (uint32_t)a); }
    void send_posture(Posture p) { send_u32(CL_PILOTING, 1, (uint32_t)p); }

    // ---- datagrams from the drone ---------------------------------------------------------
    // One datagram may carry several frames back to back.
    void on_datagram(const uint8_t *d, size_t n) {
        while (n >= (size_t)HDR) {
            uint8_t type = d[0], id = d[1], seq = d[2];
            uint32_t size = d[3] | (d[4] << 8) | (d[5] << 16) | ((uint32_t)d[6] << 24);
            if (size < (uint32_t)HDR || size > n) return;  // malformed: drop the rest
            handle(type, id, seq, d + HDR, size - HDR);
            d += size;
            n -= size;
        }
    }

    // the c2d port from the discovery reply (JSON), or `def` when it is not there
    static int parse_c2d_port(const char *json, int def) {
        const char *k = strstr(json, "\"c2d_port\"");
        if (!k) return def;
        k = strchr(k, ':');
        if (!k) return def;
        int v = atoi(k + 1);
        return v > 0 && v < 65536 ? v : def;
    }

    uint32_t frames_ok = 0, frames_dropped = 0;

private:
    Listener *L;
    uint8_t seq_[256];
    // video reassembly: fragments must arrive in order within a frame; a gap drops the frame
    uint8_t jpeg_[MAX_JPEG];
    size_t jlen_ = 0;
    int vframe_ = -1, vnext_ = 0, vtotal_ = 0;
    bool vbad_ = false;
    uint8_t mask_[16];

    static int clamp(int v) { return v > 100 ? 100 : (v < -100 ? -100 : v); }

    void out(uint8_t type, uint8_t id, const uint8_t *p, size_t n) {
        uint8_t buf[64];
        size_t len = frame(buf, type, id, p, n);
        L->send(buf, len);
    }
    void send_u32(uint8_t cls, uint8_t cmd, uint32_t v) {
        uint8_t p[8] = {PRJ_SUMO, cls, cmd, 0, (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        out(FT_DATA_ACK, ID_C2D_ACK, p, 8);
    }
    void reset_video() { jlen_ = 0; vnext_ = 0; vtotal_ = 0; vbad_ = false; memset(mask_, 0, sizeof(mask_)); }

    void handle(uint8_t type, uint8_t id, uint8_t seq, const uint8_t *p, size_t n) {
        if (type == FT_DATA_ACK) {                    // the drone wants an ack for this one
            uint8_t a = seq;
            uint8_t buf[16];
            L->send(buf, frame(buf, FT_ACK, (uint8_t)(id + ACK_ID_OFFSET), &a, 1));
        }
        if (id == ID_PING) {                          // keep-alive: echo the payload on the pong buffer
            uint8_t buf[32];
            if (n <= 16) L->send(buf, frame(buf, FT_DATA, ID_PONG, p, n));
        } else if (id == ID_D2C_VIDEO) {
            video(p, n);
        } else if (id == ID_D2C_EVENT && n >= 4) {
            event(p, n);
        }
    }

    void event(const uint8_t *p, size_t n) {
        uint8_t prj = p[0], cls = p[1];
        uint16_t cmd = p[2] | (p[3] << 8);
        if (prj == PRJ_COMMON && cls == CL_COMMONSTATE && cmd == 1 && n >= 5) L->on_battery(p[4]);
        else if (prj == PRJ_SUMO && n >= 8) {
            uint32_t v = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
            if (cls == CL_PILOTINGSTATE && cmd == 0) L->on_posture((int)v);
            else if (cls == CL_PILOTINGSTATE && cmd == 1) L->on_alert((int)v);
            else if (cls == CL_ANIMATIONSSTATE && cmd == 0) L->on_jump_load((int)v);
        }
    }

    // payload: frame number u16, flags u8, fragment number u8, fragments per frame u8, data
    void video(const uint8_t *p, size_t n) {
        if (n < 5) return;
        int frame_no = p[0] | (p[1] << 8), frag = p[3], total = p[4];
        const uint8_t *data = p + 5;
        size_t dl = n - 5;
        if (frame_no != vframe_) {                    // a new frame starts
            if (vframe_ >= 0 && !vbad_ && vtotal_ && vnext_ == vtotal_) { /* already delivered */ }
            else if (vframe_ >= 0 && jlen_) frames_dropped++;
            vframe_ = frame_no;
            reset_video();
            vtotal_ = total;
        }
        if (frag < MAX_FRAGMENTS) mask_[frag / 8] |= (uint8_t)(1u << (frag % 8));
        ack_video(frame_no);
        if (vbad_) return;
        if (frag != vnext_ || jlen_ + dl > MAX_JPEG) { vbad_ = true; return; }
        memcpy(jpeg_ + jlen_, data, dl);
        jlen_ += dl;
        vnext_++;
        if (vnext_ == vtotal_) { frames_ok++; L->on_jpeg(jpeg_, jlen_); }
    }

    // frame number, then the 128-bit fragment mask: high 64 bits first, then the low 64 (as the drone expects)
    void ack_video(int frame_no) {
        uint8_t p[18];
        p[0] = frame_no; p[1] = frame_no >> 8;
        memcpy(p + 2, mask_ + 8, 8);
        memcpy(p + 10, mask_, 8);
        out(FT_DATA, ID_C2D_VIDEOACK, p, sizeof(p));
    }
};

}  // namespace sumo
