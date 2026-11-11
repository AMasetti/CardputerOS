// Minimal MQTT 3.1.1 framing for the uplink to the Meshtastic broker: CONNECT, PUBLISH (QoS 0),
// PINGREQ and reading the fixed header of what comes back. No hardware here (host-tested).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mqtt {

constexpr const char *BROKER = "mqtt.meshtastic.org";
constexpr uint16_t PORT = 1883;
constexpr const char *USER = "meshdev";      // public credentials of the Meshtastic broker
constexpr const char *PASS = "large4cats";
constexpr uint16_t KEEPALIVE_S = 60;

enum : uint8_t { CONNECT = 0x10, CONNACK = 0x20, PUBLISH = 0x30, PINGREQ = 0xC0, PINGRESP = 0xD0, DISCONNECT = 0xE0 };

struct Out {
    uint8_t *b;
    size_t cap, n = 0;
    bool ok = true;
    Out(uint8_t *buf, size_t c) : b(buf), cap(c) {}
    void u8(uint8_t v) { if (n < cap) b[n++] = v; else ok = false; }
    void u16(uint16_t v) { u8(v >> 8); u8(v & 0xFF); }
    void str(const char *s) { size_t l = strlen(s); u16((uint16_t)l); for (size_t i = 0; i < l; i++) u8((uint8_t)s[i]); }
    void raw(const uint8_t *p, size_t l) { for (size_t i = 0; i < l; i++) u8(p[i]); }
};

inline void remaining_length(Out &o, size_t len) {
    do {
        uint8_t d = len % 128;
        len /= 128;
        o.u8(len ? d | 0x80 : d);
    } while (len);
}

inline size_t connect(const char *client_id, const char *user, const char *pass, uint8_t *buf, size_t cap) {
    size_t body = 10 + 2 + strlen(client_id) + 2 + strlen(user) + 2 + strlen(pass);
    Out o(buf, cap);
    o.u8(CONNECT);
    remaining_length(o, body);
    o.str("MQTT");
    o.u8(4);              // protocol level 3.1.1
    o.u8(0xC2);           // username, password, clean session
    o.u16(KEEPALIVE_S);
    o.str(client_id);
    o.str(user);
    o.str(pass);
    return o.ok ? o.n : 0;
}

inline size_t publish(const char *topic, const uint8_t *payload, size_t len, uint8_t *buf, size_t cap) {
    Out o(buf, cap);
    o.u8(PUBLISH);
    remaining_length(o, 2 + strlen(topic) + len);
    o.str(topic);
    o.raw(payload, len);
    return o.ok ? o.n : 0;
}

inline size_t pingreq(uint8_t *buf) { buf[0] = PINGREQ; buf[1] = 0; return 2; }

// CONNACK accepted? (bytes as received: 20 02 <flags> <rc>)
inline bool connack_ok(const uint8_t *b, size_t n) { return n >= 4 && b[0] == CONNACK && b[1] == 2 && b[3] == 0; }

} // namespace mqtt
