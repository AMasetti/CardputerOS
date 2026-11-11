// Meshtastic protocol core: wire format, encryption, channel/frequency math and a flooding router.
//
// No hardware here: the app gives it the radio (send/busy), a clock, random numbers and AES; the
// host test (test/mesh_test.cpp) runs two routers against each other and checks the bytes against
// the Meshtastic reference values. Compatible with Meshtastic firmware 2.x on the air:
//
//   16-byte header (to, from, id, flags, channel hash, next hop, relay node), little endian
//   payload = protobuf Data, AES-CTR with the channel key, nonce = id (u64 LE) | from (u32 LE) | 0
//   LoRa sync word 0x2B, preamble 16, explicit header, CRC on
//
// Not implemented: PKI (Curve25519) direct messages, so this node does not publish a public key and
// other nodes fall back to the channel key for direct messages to it; secondary channels; MQTT.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mesh {

constexpr uint32_t BROADCAST = 0xFFFFFFFFu;
constexpr size_t HEADER_LEN = 16;
constexpr size_t MAX_PACKET = 255;
constexpr size_t MAX_PAYLOAD = MAX_PACKET - HEADER_LEN; // encrypted Data
constexpr size_t MAX_TEXT = 200;
constexpr uint8_t SYNC_WORD = 0x2B;
constexpr uint16_t PREAMBLE = 16;
constexpr uint32_t HW_M5STACK_CARDPUTER_ADV = 112; // HardwareModel
constexpr uint8_t CWMIN = 3, CWMAX = 8;

enum Port : uint32_t {
    PORT_TEXT = 1,
    PORT_POSITION = 3,
    PORT_NODEINFO = 4,
    PORT_ROUTING = 5,
    PORT_TELEMETRY = 67,
    PORT_TRACEROUTE = 70,
};

enum RoutingError : uint32_t {
    ROUTING_NONE = 0,
    ROUTING_NO_ROUTE = 1,
    ROUTING_GOT_NAK = 2,
    ROUTING_TIMEOUT = 3,
    ROUTING_MAX_RETRANSMIT = 5,
    ROUTING_NO_CHANNEL = 6,
    ROUTING_TOO_LARGE = 7,
};

// ---- protobuf ---------------------------------------------------------------------------------
struct PbWriter {
    uint8_t *buf;
    size_t cap, len = 0;
    bool ok = true;
    PbWriter(uint8_t *b, size_t c) : buf(b), cap(c) {}
    void byte(uint8_t v) {
        if (len < cap) buf[len++] = v;
        else ok = false;
    }
    void varint(uint64_t v) {
        while (v >= 0x80) { byte((uint8_t)(v | 0x80)); v >>= 7; }
        byte((uint8_t)v);
    }
    void tag(uint32_t field, uint8_t wt) { varint((uint64_t)field << 3 | wt); }
    // proto3: zero values are not written
    void u32(uint32_t f, uint32_t v) { if (v) { tag(f, 0); varint(v); } }
    void i32(uint32_t f, int32_t v) { if (v) { tag(f, 0); varint((uint64_t)(int64_t)v); } }
    void boolean(uint32_t f, bool v) { if (v) { tag(f, 0); byte(1); } }
    void fixed32(uint32_t f, uint32_t v) {
        if (!v) return;
        tag(f, 5);
        for (int i = 0; i < 4; i++) byte((uint8_t)(v >> (8 * i)));
    }
    void flt(uint32_t f, float v) {
        if (v == 0.0f) return;
        uint32_t u;
        memcpy(&u, &v, 4);
        tag(f, 5);
        for (int i = 0; i < 4; i++) byte((uint8_t)(u >> (8 * i)));
    }
    void bytes(uint32_t f, const uint8_t *p, size_t n) {
        if (!n) return;
        tag(f, 2);
        varint(n);
        for (size_t i = 0; i < n; i++) byte(p[i]);
    }
    void str(uint32_t f, const char *s) { bytes(f, (const uint8_t *)s, strlen(s)); }
};

struct PbReader {
    const uint8_t *p, *end;
    bool ok = true;
    uint8_t wt = 0;
    PbReader(const uint8_t *b, size_t n) : p(b), end(b + n) {}
    uint64_t varint() {
        uint64_t v = 0;
        for (int s = 0; s < 64; s += 7) {
            if (p >= end) { ok = false; return 0; }
            uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7F) << s;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    uint32_t fixed32() {
        if (end - p < 4) { ok = false; p = end; return 0; }
        uint32_t v = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
        p += 4;
        return v;
    }
    float flt() { uint32_t u = fixed32(); float f; memcpy(&f, &u, 4); return f; }
    // next field number, 0 at the end or on a malformed message
    uint32_t next() {
        if (!ok || p >= end) return 0;
        uint64_t k = varint();
        wt = (uint8_t)(k & 7);
        return ok ? (uint32_t)(k >> 3) : 0;
    }
    bool bytes(const uint8_t *&out, size_t &n) {
        uint64_t l = varint();
        if (!ok || l > (uint64_t)(end - p)) { ok = false; return false; }
        out = p;
        n = (size_t)l;
        p += l;
        return true;
    }
    void skip() {
        const uint8_t *b;
        size_t n;
        switch (wt) {
        case 0: varint(); break;
        case 1: if (end - p < 8) ok = false; else p += 8; break;
        case 2: bytes(b, n); break;
        case 5: fixed32(); break;
        default: ok = false;
        }
    }
    void str(char *out, size_t cap) {
        const uint8_t *b;
        size_t n;
        if (!bytes(b, n)) return;
        if (n >= cap) n = cap - 1;
        memcpy(out, b, n);
        out[n] = 0;
    }
};

// ---- messages ------------------------------------------------------------------------------------
struct Data {
    uint32_t portnum = 0;
    uint8_t payload[MAX_PAYLOAD];
    size_t payload_len = 0;
    bool want_response = false;
    uint32_t dest = 0, source = 0, request_id = 0, reply_id = 0, emoji = 0, bitfield = 0;
    bool has_bitfield = false;

    size_t encode(uint8_t *out, size_t cap) const {
        PbWriter w(out, cap);
        w.u32(1, portnum);
        w.bytes(2, payload, payload_len);
        w.boolean(3, want_response);
        w.fixed32(4, dest);
        w.fixed32(5, source);
        w.fixed32(6, request_id);
        w.fixed32(7, reply_id);
        w.fixed32(8, emoji);
        if (has_bitfield) { w.tag(9, 0); w.varint(bitfield); }
        return w.ok ? w.len : 0;
    }
    bool decode(const uint8_t *in, size_t n) {
        PbReader r(in, n);
        *this = Data();
        while (uint32_t f = r.next()) {
            if (f == 1 && r.wt == 0) portnum = (uint32_t)r.varint();
            else if (f == 2 && r.wt == 2) {
                const uint8_t *b;
                size_t l;
                if (r.bytes(b, l)) {
                    if (l > sizeof(payload)) return false;
                    memcpy(payload, b, l);
                    payload_len = l;
                }
            } else if (f == 3 && r.wt == 0) want_response = r.varint() != 0;
            else if (f == 4 && r.wt == 5) dest = r.fixed32();
            else if (f == 5 && r.wt == 5) source = r.fixed32();
            else if (f == 6 && r.wt == 5) request_id = r.fixed32();
            else if (f == 7 && r.wt == 5) reply_id = r.fixed32();
            else if (f == 8 && r.wt == 5) emoji = r.fixed32();
            else if (f == 9 && r.wt == 0) { bitfield = (uint32_t)r.varint(); has_bitfield = true; }
            else r.skip();
        }
        return r.ok && portnum != 0;
    }
};

struct User {
    char id[16] = {0};
    char long_name[40] = {0};
    char short_name[8] = {0};
    uint32_t hw_model = 0;
    uint32_t role = 0;
    bool is_licensed = false;
    bool has_public_key = false;

    size_t encode(uint8_t *out, size_t cap) const {
        PbWriter w(out, cap);
        w.str(1, id);
        w.str(2, long_name);
        w.str(3, short_name);
        w.u32(5, hw_model);
        w.boolean(6, is_licensed);
        w.u32(7, role);
        return w.ok ? w.len : 0;
    }
    bool decode(const uint8_t *in, size_t n) {
        PbReader r(in, n);
        *this = User();
        while (uint32_t f = r.next()) {
            if (f == 1 && r.wt == 2) r.str(id, sizeof(id));
            else if (f == 2 && r.wt == 2) r.str(long_name, sizeof(long_name));
            else if (f == 3 && r.wt == 2) r.str(short_name, sizeof(short_name));
            else if (f == 5 && r.wt == 0) hw_model = (uint32_t)r.varint();
            else if (f == 6 && r.wt == 0) is_licensed = r.varint() != 0;
            else if (f == 7 && r.wt == 0) role = (uint32_t)r.varint();
            else if (f == 8 && r.wt == 2) { const uint8_t *b; size_t l; if (r.bytes(b, l)) has_public_key = l == 32; }
            else r.skip();
        }
        return r.ok;
    }
};

struct Position {
    int32_t latitude_i = 0, longitude_i = 0; // degrees * 1e7
    int32_t altitude = 0;                    // m
    uint32_t time = 0;                       // unix seconds
    uint32_t location_source = 0;            // 2 = internal GPS
    uint32_t sats_in_view = 0;
    uint32_t precision_bits = 0;
    uint32_t ground_speed = 0;               // m/s
    bool has_pos = false;

    size_t encode(uint8_t *out, size_t cap) const {
        PbWriter w(out, cap);
        // sfixed32: always written when there is a position, 0 is a valid coordinate
        if (has_pos) {
            w.tag(1, 5);
            for (int i = 0; i < 4; i++) w.byte((uint8_t)((uint32_t)latitude_i >> (8 * i)));
            w.tag(2, 5);
            for (int i = 0; i < 4; i++) w.byte((uint8_t)((uint32_t)longitude_i >> (8 * i)));
        }
        w.i32(3, altitude);
        w.fixed32(4, time);
        w.u32(5, location_source);
        w.u32(15, ground_speed);
        w.u32(19, sats_in_view);
        w.u32(23, precision_bits);
        return w.ok ? w.len : 0;
    }
    bool decode(const uint8_t *in, size_t n) {
        PbReader r(in, n);
        *this = Position();
        bool lat = false, lon = false;
        while (uint32_t f = r.next()) {
            if (f == 1 && r.wt == 5) { latitude_i = (int32_t)r.fixed32(); lat = true; }
            else if (f == 2 && r.wt == 5) { longitude_i = (int32_t)r.fixed32(); lon = true; }
            else if (f == 3 && r.wt == 0) altitude = (int32_t)r.varint();
            else if (f == 4 && r.wt == 5) time = r.fixed32();
            else if (f == 5 && r.wt == 0) location_source = (uint32_t)r.varint();
            else if (f == 15 && r.wt == 0) ground_speed = (uint32_t)r.varint();
            else if (f == 19 && r.wt == 0) sats_in_view = (uint32_t)r.varint();
            else if (f == 23 && r.wt == 0) precision_bits = (uint32_t)r.varint();
            else r.skip();
        }
        has_pos = lat && lon && (latitude_i || longitude_i);
        return r.ok;
    }
};

struct DeviceMetrics {
    uint32_t battery_level = 0; // 0..100, 101 = powered
    float voltage = 0, channel_utilization = 0, air_util_tx = 0;
    uint32_t uptime_seconds = 0;
    bool has = false;
};

// Telemetry: only the device metrics variant
inline size_t encode_telemetry(uint32_t time, const DeviceMetrics &m, uint8_t *out, size_t cap) {
    uint8_t inner[40];
    PbWriter d(inner, sizeof(inner));
    d.u32(1, m.battery_level);
    d.flt(2, m.voltage);
    d.flt(3, m.channel_utilization);
    d.flt(4, m.air_util_tx);
    d.u32(5, m.uptime_seconds);
    PbWriter w(out, cap);
    w.fixed32(1, time);
    w.tag(2, 2);
    w.varint(d.len);
    for (size_t i = 0; i < d.len; i++) w.byte(inner[i]);
    return w.ok && d.ok ? w.len : 0;
}

inline bool decode_telemetry(const uint8_t *in, size_t n, DeviceMetrics &m) {
    PbReader r(in, n);
    m = DeviceMetrics();
    while (uint32_t f = r.next()) {
        if (f == 2 && r.wt == 2) {
            const uint8_t *b;
            size_t l;
            if (!r.bytes(b, l)) break;
            PbReader d(b, l);
            m.has = true;
            while (uint32_t g = d.next()) {
                if (g == 1 && d.wt == 0) m.battery_level = (uint32_t)d.varint();
                else if (g == 2 && d.wt == 5) m.voltage = d.flt();
                else if (g == 3 && d.wt == 5) m.channel_utilization = d.flt();
                else if (g == 4 && d.wt == 5) m.air_util_tx = d.flt();
                else if (g == 5 && d.wt == 0) m.uptime_seconds = (uint32_t)d.varint();
                else d.skip();
            }
        } else r.skip();
    }
    return r.ok && m.has;
}

// Routing: error_reason only (an ack is error_reason NONE with Data.request_id = acked packet)
inline size_t encode_routing(uint32_t error, uint8_t *out, size_t cap) {
    PbWriter w(out, cap);
    // NONE is 0, which proto3 would drop; Meshtastic sends the field anyway so the message is never empty
    w.tag(3, 0);
    w.varint(error);
    return w.ok ? w.len : 0;
}

inline bool decode_routing(const uint8_t *in, size_t n, uint32_t &error) {
    PbReader r(in, n);
    error = ROUTING_NONE;
    while (uint32_t f = r.next()) {
        if (f == 3 && r.wt == 0) error = (uint32_t)r.varint();
        else r.skip();
    }
    return r.ok;
}

// ---- header ---------------------------------------------------------------------------------------
struct Header {
    uint32_t to = 0, from = 0, id = 0;
    uint8_t flags = 0, channel = 0, next_hop = 0, relay_node = 0;

    uint8_t hop_limit() const { return flags & 0x07; }
    bool want_ack() const { return flags & 0x08; }
    bool via_mqtt() const { return flags & 0x10; }
    uint8_t hop_start() const { return flags >> 5; }
    void set_hop_limit(uint8_t h) { flags = (flags & ~0x07) | (h & 0x07); }
    static uint8_t make_flags(uint8_t hop_limit, bool want_ack, uint8_t hop_start) {
        return (hop_limit & 7) | (want_ack ? 0x08 : 0) | ((hop_start & 7) << 5);
    }
    void write(uint8_t *b) const {
        for (int i = 0; i < 4; i++) {
            b[i] = (uint8_t)(to >> (8 * i));
            b[4 + i] = (uint8_t)(from >> (8 * i));
            b[8 + i] = (uint8_t)(id >> (8 * i));
        }
        b[12] = flags;
        b[13] = channel;
        b[14] = next_hop;
        b[15] = relay_node;
    }
    void read(const uint8_t *b) {
        to = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
        from = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
        id = b[8] | b[9] << 8 | b[10] << 16 | (uint32_t)b[11] << 24;
        flags = b[12];
        channel = b[13];
        next_hop = b[14];
        relay_node = b[15];
    }
};

// ---- crypto ------------------------------------------------------------------------------------------
// AES block encryption supplied by the platform: key of `bits` (128/256), one 16-byte block
typedef int (*AesEcbFn)(const uint8_t *key, int bits, const uint8_t in[16], uint8_t out[16]);

static const uint8_t DEFAULT_PSK[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                        0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

// AES-CTR in place. Counter = the last 4 bytes of the nonce, big endian (Meshtastic's CTR<AES>).
inline void ctr_crypt(AesEcbFn aes, const uint8_t *key, size_t keylen, uint32_t from, uint32_t id, uint8_t *buf, size_t n) {
    if (!keylen) return;
    uint8_t ctr[16] = {0}, ks[16];
    for (int i = 0; i < 4; i++) {
        ctr[i] = (uint8_t)(id >> (8 * i)); // packet id as u64 LE: high half is 0
        ctr[8 + i] = (uint8_t)(from >> (8 * i));
    }
    for (size_t off = 0; off < n; off += 16) {
        aes(key, (int)keylen * 8, ctr, ks);
        for (size_t i = 0; i < 16 && off + i < n; i++) buf[off + i] ^= ks[i];
        for (int i = 15; i >= 12; i--)
            if (++ctr[i]) break;
    }
}

// Channel PSK as stored/shared (0, 1, 16 or 32 bytes) -> key used on the air
inline size_t expand_psk(const uint8_t *psk, size_t n, uint8_t out[32]) {
    memset(out, 0, 32);
    if (n == 0) return 0;
    if (n == 1) {
        if (psk[0] == 0) return 0; // no encryption
        memcpy(out, DEFAULT_PSK, 16);
        out[15] = (uint8_t)(out[15] + psk[0] - 1); // "simple" keys 1..10
        return 16;
    }
    if (n <= 16) { memcpy(out, psk, n); return 16; }
    memcpy(out, psk, n > 32 ? 32 : n);
    return 32;
}

inline uint8_t xor_hash(const uint8_t *p, size_t n) {
    uint8_t h = 0;
    for (size_t i = 0; i < n; i++) h ^= p[i];
    return h;
}

inline uint8_t channel_hash(const char *name, const uint8_t *key, size_t keylen) {
    return xor_hash((const uint8_t *)name, strlen(name)) ^ xor_hash(key, keylen);
}

// djb2, as the firmware uses to pick the frequency slot from the channel name
inline uint32_t djb2(const char *s) {
    uint32_t h = 5381;
    while (*s) h = h * 33 + (uint8_t)*s++;
    return h;
}

// ---- base64 (channel keys are shared as base64) ------------------------------------------------------
inline size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        char c[4] = {T[v >> 18 & 63], T[v >> 12 & 63], i + 1 < n ? T[v >> 6 & 63] : '=', i + 2 < n ? T[v & 63] : '='};
        for (int k = 0; k < 4; k++) if (o + 1 < cap) out[o++] = c[k];
    }
    if (cap) out[o] = 0;
    return o;
}

inline int b64_decode(const char *in, uint8_t *out, size_t cap) {
    uint32_t v = 0;
    int bits = 0;
    size_t o = 0;
    for (; *in && *in != '='; in++) {
        char c = *in;
        int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : (c == '+' || c == '-') ? 62 : (c == '/' || c == '_') ? 63 : -1;
        if (d < 0) return -1;
        v = v << 6 | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(v >> bits);
        }
    }
    return (int)o;
}

// ---- regions and modem presets ---------------------------------------------------------------------
struct Region {
    const char *name;
    uint32_t start_khz, end_khz;
    int8_t max_power; // dBm allowed by the band plan
    const char *note;
};

// Argentina: ENACOM allows spread-spectrum devices in 915-928 MHz, which is Meshtastic's ANZ plan
// (and where the Argentine mesh lives). US (902-928) is offered for groups that use it, but its
// default LongFast slot (906.875 MHz) falls in a segment Argentina assigned to other services.
static const Region REGIONS[] = {
    {"ANZ", 915000, 928000, 30, "Argentina/Brasil/AU/NZ 915-928"},
    {"US", 902000, 928000, 30, "EE.UU. 902-928"},
};
constexpr int NUM_REGIONS = sizeof(REGIONS) / sizeof(REGIONS[0]);

struct Preset {
    const char *name; // display name, also the default channel name
    uint32_t bw_hz;
    uint8_t sf, cr;
};

// ModemPreset enum order of the firmware is not needed: presets are kept by name
static const Preset PRESETS[] = {
    {"LongFast", 250000, 11, 5},   {"LongSlow", 125000, 12, 8},   {"LongMod", 125000, 11, 8},
    {"LongTurbo", 500000, 11, 8},  {"MediumSlow", 250000, 10, 5}, {"MediumFast", 250000, 9, 5},
    {"ShortSlow", 250000, 8, 5},   {"ShortFast", 250000, 7, 5},   {"ShortTurbo", 500000, 7, 5},
};
constexpr int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

inline uint32_t num_slots(const Region &r, const Preset &p) {
    uint32_t span = (r.end_khz - r.start_khz) * 1000u;
    return (span + p.bw_hz / 2) / p.bw_hz; // round()
}

// slot: 0 = from the channel name hash, n = slot n (1-based, like lora.channel_num)
inline uint32_t frequency_hz(const Region &r, const Preset &p, const char *channel_name, uint32_t slot) {
    uint32_t n = num_slots(r, p);
    uint32_t s = (slot && slot <= n) ? slot - 1 : (n ? djb2(channel_name) % n : 0);
    return r.start_khz * 1000u + p.bw_hz / 2 + s * p.bw_hz;
}

// time on air in ms for len bytes (explicit header, CRC on)
inline uint32_t airtime_ms(const Preset &p, size_t len, uint16_t preamble = PREAMBLE) {
    uint32_t sf = p.sf;
    bool ldro = (((uint64_t)1000000u << sf) / p.bw_hz) >= 16000;
    int32_t num = 8 * (int32_t)len - 4 * (int32_t)sf + 28 + 16;
    int32_t den = 4 * ((int32_t)sf - (ldro ? 2 : 0));
    int32_t n = num > 0 ? (num + den - 1) / den : 0;
    // preamble + 4.25 + 8 + payload symbols, in quarter symbols
    uint64_t q = (uint64_t)preamble * 4 + 17 + 32 + (uint64_t)n * p.cr * 4;
    return (uint32_t)(((q << sf) * 1000u / p.bw_hz + 3) / 4);
}

// contention slot (ms): CAD time (2 symbols + 0.5) plus propagation/turnaround/MAC
inline uint32_t slot_time_ms(const Preset &p) {
    uint32_t sym_us = (uint32_t)(((uint64_t)1000000u << p.sf) / p.bw_hz);
    return (sym_us * 5 / 2 + 7600) / 1000;
}

// ---- node database --------------------------------------------------------------------------------------
struct Node {
    uint32_t num = 0;
    User user;
    bool has_user = false;
    uint32_t last_heard = 0; // app clock, ms
    float snr = 0;
    int16_t rssi = 0;
    int8_t hops = -1;        // hops away, -1 unknown
    bool via_mqtt = false;
    Position pos;
    DeviceMetrics metrics;
    uint32_t info_asked = 0; // when we last asked it for its NodeInfo
};

// ---- the router -------------------------------------------------------------------------------------------
// What the router tells the app about
class Listener {
  public:
    virtual void on_text(const Header &h, const Data &d, const Node *from) = 0;
    virtual void on_ack(uint32_t id, bool ok, uint32_t error) = 0; // error: RoutingError
    virtual void on_node(const Node &n) = 0;                         // node info, position or metrics changed
    virtual ~Listener() {}
};

struct Platform {
    uint32_t (*now)(void);                         // ms
    uint32_t (*rnd)(void);
    AesEcbFn aes;
    int (*send)(const uint8_t *p, size_t n);       // hand a packet to the radio, 0 = queued
    int (*busy)(void);                             // the radio is receiving or sending
};

class Router {
  public:
    static constexpr int MAX_NODES = 80;
    static constexpr int SEEN = 96;
    static constexpr int OUTQ = 12;
    static constexpr int PENDING = 8;
    static constexpr uint32_t SEEN_MS = 10u * 60 * 1000;
    static constexpr int RETRIES = 3;

    // settings (the app sets them, then calls set_channel)
    uint32_t me = 0;
    uint8_t hop_limit = 3;
    bool rebroadcast = true;
    User owner;
    Preset preset = PRESETS[0];

    Node nodes[MAX_NODES];
    int num_nodes = 0;

    struct Stats { uint32_t rx = 0, rx_dup = 0, rx_foreign = 0, relayed = 0, sent = 0, acks = 0; } stats;

    void begin(const Platform &p, Listener *l) {
        pf_ = p;
        listener_ = l;
    }

    // name "" = the preset name (the default public channel); psk as shared (0/1/16/32 bytes)
    void set_channel(const char *name, const uint8_t *psk, size_t psk_len) {
        strncpy(chan_name_, name && *name ? name : preset.name, sizeof(chan_name_) - 1);
        chan_name_[sizeof(chan_name_) - 1] = 0;
        key_len_ = expand_psk(psk, psk_len, key_);
        hash_ = channel_hash(chan_name_, key_, key_len_);
    }
    const char *channel_name() const { return chan_name_; }
    uint8_t channel_hash_value() const { return hash_; }

    Node *find(uint32_t num) {
        for (int i = 0; i < num_nodes; i++)
            if (nodes[i].num == num) return &nodes[i];
        return nullptr;
    }

    Node *get_or_add(uint32_t num) {
        if (Node *n = find(num)) return n;
        if (num_nodes == MAX_NODES) { // forget the node heard least recently
            int old = 0;
            for (int i = 1; i < num_nodes; i++)
                if ((int32_t)(nodes[i].last_heard - nodes[old].last_heard) < 0) old = i;
            nodes[old] = nodes[--num_nodes];
        }
        Node &n = nodes[num_nodes++];
        n = Node();
        n.num = num;
        return &n;
    }

    // ---- sending ----
    // returns the packet id (0 when it could not be queued)
    uint32_t send_data(uint32_t to, Data &d, bool want_ack, uint32_t delay_ms = 0) {
        uint8_t buf[MAX_PACKET];
        size_t n = d.encode(buf + HEADER_LEN, MAX_PAYLOAD);
        if (!n) return 0;
        Header h;
        h.to = to;
        h.from = me;
        h.id = new_id();
        h.flags = Header::make_flags(hop_limit, want_ack, hop_limit);
        h.channel = hash_;
        h.next_hop = 0;
        h.relay_node = (uint8_t)(me & 0xFF);
        h.write(buf);
        ctr_crypt(pf_.aes, key_, key_len_, h.from, h.id, buf + HEADER_LEN, n);
        uint32_t delay = delay_ms + (pf_.rnd() % (1u << CWMIN)) * slot_time_ms(preset);
        if (!enqueue(buf, HEADER_LEN + n, delay, false, h.from, h.id)) return 0;
        // DMs wait for the routing ack and are sent again; broadcasts are acked implicitly when a
        // neighbour relays them (no retries, so a lone node does not keep the channel busy)
        if (want_ack || to == BROADCAST) track(h, buf, HEADER_LEN + n, want_ack && to != BROADCAST);
        stats.sent++;
        return h.id;
    }

    uint32_t send_text(uint32_t to, const char *text) {
        Data d;
        d.portnum = PORT_TEXT;
        d.payload_len = strnlen(text, MAX_TEXT);
        memcpy(d.payload, text, d.payload_len);
        return send_data(to, d, to != BROADCAST);
    }

    uint32_t send_nodeinfo(uint32_t to = BROADCAST, bool want_response = false) {
        Data d;
        d.portnum = PORT_NODEINFO;
        d.payload_len = owner.encode(d.payload, sizeof(d.payload));
        d.want_response = want_response;
        return send_data(to, d, false);
    }

    uint32_t send_position(const Position &p, uint32_t to = BROADCAST) {
        Data d;
        d.portnum = PORT_POSITION;
        d.payload_len = p.encode(d.payload, sizeof(d.payload));
        return send_data(to, d, false);
    }

    uint32_t send_telemetry(uint32_t time, const DeviceMetrics &m) {
        Data d;
        d.portnum = PORT_TELEMETRY;
        d.payload_len = encode_telemetry(time, m, d.payload, sizeof(d.payload));
        return d.payload_len ? send_data(BROADCAST, d, false) : 0;
    }

    // ---- receiving ----
    void on_receive(const uint8_t *raw, size_t len, int16_t rssi, float snr) {
        if (len < HEADER_LEN || len > MAX_PACKET) return;
        Header h;
        h.read(raw);
        stats.rx++;
        if (h.from == me) { // our own packet relayed by a neighbour: implicit ack
            implicit_ack(h.id);
            return;
        }
        if (seen(h.from, h.id)) {
            stats.rx_dup++;
            cancel_relay(h.from, h.id); // someone else relayed it first
            if (h.to == BROADCAST) implicit_ack(h.id);
            return;
        }
        remember(h.from, h.id);

        Data d;
        bool decoded = false;
        if (h.channel == hash_) {
            uint8_t plain[MAX_PAYLOAD];
            size_t n = len - HEADER_LEN;
            memcpy(plain, raw + HEADER_LEN, n);
            ctr_crypt(pf_.aes, key_, key_len_, h.from, h.id, plain, n);
            decoded = d.decode(plain, n);
        }
        if (!decoded) stats.rx_foreign++;

        if (decoded && (h.to == me || h.to == BROADCAST)) deliver(h, d, rssi, snr);

        // relay: everything not addressed to us, while hops remain (managed flooding)
        if (rebroadcast && h.to != me && h.hop_limit() > 0 && (h.next_hop == 0 || h.next_hop == (uint8_t)(me & 0xFF))) {
            uint8_t buf[MAX_PACKET];
            memcpy(buf, raw, len);
            Header r = h;
            r.set_hop_limit(h.hop_limit() - 1);
            r.relay_node = (uint8_t)(me & 0xFF);
            r.next_hop = 0;
            r.write(buf);
            // low SNR (far away) relays first, close nodes wait: the one that adds the most range goes first
            int cw = snr <= -20 ? CWMIN : snr >= 10 ? CWMAX : CWMIN + (int)((snr + 20) * (CWMAX - CWMIN) / 30);
            uint32_t slot = slot_time_ms(preset);
            uint32_t delay = 2 * CWMAX * slot + (pf_.rnd() % (1u << cw)) * slot;
            if (enqueue(buf, len, delay, true, h.from, h.id)) stats.relayed++;
        }
    }

    // call often: sends what is due, retries DMs
    void loop() {
        uint32_t t = pf_.now();
        for (int i = 0; i < PENDING; i++) {
            Pending &p = pending_[i];
            if (!p.used || (int32_t)(t - p.next) < 0) continue;
            if (p.retries_left > 0) {
                p.retries_left--;
                p.next = t + retry_ms(p.len);
                enqueue(p.buf, p.len, 0, false, me, p.id);
            } else {
                p.used = false;
                if (p.explicit_ack) listener_->on_ack(p.id, false, ROUTING_MAX_RETRANSMIT);
            }
        }
        if (pf_.busy()) return;
        for (int i = 0; i < OUTQ; i++) {
            Out &o = out_[i];
            if (!o.used || (int32_t)(t - o.due) < 0) continue;
            if (pf_.send(o.buf, o.len) == 0) o.used = false;
            return; // one at a time, then wait for the radio again
        }
    }

    int queued() const {
        int n = 0;
        for (int i = 0; i < OUTQ; i++) n += out_[i].used;
        return n;
    }

  private:
    struct Seen { uint32_t from, id, at; };
    struct Out { bool used = false, relay = false; uint32_t due = 0, from = 0, id = 0; size_t len = 0; uint8_t buf[MAX_PACKET]; };
    struct Pending { bool used = false, explicit_ack = false; uint32_t id = 0, next = 0; int retries_left = 0; size_t len = 0; uint8_t buf[MAX_PACKET]; };

    Platform pf_{};
    Listener *listener_ = nullptr;
    char chan_name_[16] = "LongFast";
    uint8_t key_[32] = {0};
    size_t key_len_ = 0;
    uint8_t hash_ = 0;
    Seen seen_[SEEN] = {};
    int seen_pos_ = 0;
    Out out_[OUTQ];
    Pending pending_[PENDING];
    uint32_t last_info_ask_ = 0;

    // ms since `at` (timestamps are stored with bit 0 set so 0 means "never"; that can put them 1 ms ahead)
    static uint32_t since(uint32_t t, uint32_t at) { return (int32_t)(t - at) < 0 ? 0 : t - at; }

    uint32_t new_id() {
        uint32_t id;
        do id = pf_.rnd(); while (id == 0);
        return id;
    }

    bool seen(uint32_t from, uint32_t id) {
        uint32_t t = pf_.now();
        for (auto &s : seen_)
            if (s.id == id && s.from == from && s.at && since(t, s.at) < SEEN_MS) return true;
        return false;
    }
    void remember(uint32_t from, uint32_t id) {
        seen_[seen_pos_] = {from, id, pf_.now() | 1};
        seen_pos_ = (seen_pos_ + 1) % SEEN;
    }

    bool enqueue(const uint8_t *b, size_t n, uint32_t delay, bool relay, uint32_t from, uint32_t id) {
        for (auto &o : out_) {
            if (o.used) continue;
            o.used = true;
            o.relay = relay;
            o.due = pf_.now() + delay;
            o.from = from;
            o.id = id;
            o.len = n;
            memcpy(o.buf, b, n);
            return true;
        }
        return false;
    }

    void cancel_relay(uint32_t from, uint32_t id) {
        for (auto &o : out_)
            if (o.used && o.relay && o.from == from && o.id == id) o.used = false;
    }

    uint32_t retry_ms(size_t len) {
        // the firmware's retransmission time: two airtimes, the contention windows and processing time
        uint32_t a = airtime_ms(preset, len);
        uint32_t slot = slot_time_ms(preset);
        return 2 * a + ((1u << CWMIN) + 2 * CWMAX + (1u << ((CWMAX + CWMIN) / 2))) * slot + 4500;
    }

    void track(const Header &h, const uint8_t *b, size_t n, bool explicit_ack) {
        for (auto &p : pending_) {
            if (p.used) continue;
            p.used = true;
            p.explicit_ack = explicit_ack;
            p.id = h.id;
            p.retries_left = explicit_ack ? RETRIES : 0;
            p.next = pf_.now() + (explicit_ack ? retry_ms(n) : 60000);
            p.len = n;
            memcpy(p.buf, b, n);
            return;
        }
    }

    void implicit_ack(uint32_t id) {
        for (auto &p : pending_)
            if (p.used && p.id == id && !p.explicit_ack) {
                p.used = false;
                listener_->on_ack(id, true, ROUTING_NONE);
            }
    }

    void explicit_ack(uint32_t id, uint32_t error) {
        for (auto &p : pending_)
            if (p.used && p.id == id) {
                p.used = false;
                listener_->on_ack(id, error == ROUTING_NONE, error);
            }
        // the packet may still be waiting in the queue for a retry
        for (auto &o : out_)
            if (o.used && !o.relay && o.from == me && o.id == id) o.used = false;
    }

    void send_ack(const Header &h) {
        Data d;
        d.portnum = PORT_ROUTING;
        d.payload_len = encode_routing(ROUTING_NONE, d.payload, sizeof(d.payload));
        d.request_id = h.id;
        // enough hops to get back the way it came
        uint8_t saved = hop_limit;
        if (h.hop_start()) {
            uint8_t used = h.hop_start() - h.hop_limit();
            hop_limit = used + 1 > saved ? saved : (uint8_t)(used + 1);
            if (hop_limit < 1) hop_limit = 1;
        }
        send_data(h.from, d, false);
        hop_limit = saved;
        stats.acks++;
    }

    void deliver(const Header &h, const Data &d, int16_t rssi, float snr) {
        Node *n = get_or_add(h.from);
        n->last_heard = pf_.now() | 1;
        n->rssi = rssi;
        n->snr = snr;
        n->via_mqtt = h.via_mqtt();
        if (h.hop_start() && h.hop_start() >= h.hop_limit()) n->hops = (int8_t)(h.hop_start() - h.hop_limit());
        bool changed = false;

        switch (d.portnum) {
        case PORT_TEXT:
            listener_->on_text(h, d, n);
            break;
        case PORT_NODEINFO: {
            User u;
            if (u.decode(d.payload, d.payload_len)) {
                n->user = u;
                n->has_user = true;
                changed = true;
                if (d.want_response && h.to == me) send_nodeinfo(h.from, false);
            }
            break;
        }
        case PORT_POSITION: {
            Position p;
            if (p.decode(d.payload, d.payload_len) && p.has_pos) {
                n->pos = p;
                changed = true;
            }
            break;
        }
        case PORT_TELEMETRY: {
            DeviceMetrics m;
            if (decode_telemetry(d.payload, d.payload_len, m)) {
                n->metrics = m;
                changed = true;
            }
            break;
        }
        case PORT_ROUTING: {
            uint32_t err;
            if (h.to == me && d.request_id && decode_routing(d.payload, d.payload_len, err)) explicit_ack(d.request_id, err);
            break;
        }
        default:
            break;
        }
        if (h.to == me && h.want_ack() && d.portnum != PORT_ROUTING) send_ack(h);
        // a node we know nothing about: ask for its NodeInfo (at most one ask every 30 s, each node every 10 min)
        uint32_t t = pf_.now();
        if (!n->has_user && d.portnum != PORT_NODEINFO && (!n->info_asked || since(t, n->info_asked) > 600000) &&
            (!last_info_ask_ || since(t, last_info_ask_) > 30000)) {
            n->info_asked = t | 1;
            last_info_ask_ = t | 1;
            send_nodeinfo(h.from, true);
        }
        listener_->on_node(*n);
        (void)changed;
    }
};

} // namespace mesh
