// Meshtastic "phone API": what the official apps (iOS, Android, Python) speak to a node, over BLE
// (ToRadio/FromRadio characteristics) or TCP (port 4403, 0x94 0xC3 framing). Transport-agnostic and
// host-tested (test/phone_test.py drives it with the official Python client over TCP).
//
// ToRadio  (phone -> node): want_config_id, packet (MeshPacket with decoded Data), heartbeat, disconnect
// FromRadio(node -> phone): my_info, own node_info, metadata, channels, config, module config,
//                           other node_infos, config_complete_id, then live packets and queue status
//
// Config from the phone (AdminMessage to the local node): owner, LoRa, primary channel and time are
// applied through AdminHooks; get_* requests are answered; other settings are accepted and ignored.
#pragma once
#include <stdio.h>
#include "mesh_core.h"

namespace phone {
using namespace mesh;

constexpr uint32_t NONCE_ONLY_CONFIG = 69420, NONCE_ONLY_NODES = 69421;
constexpr size_t MAX_FROM_RADIO = 512;
constexpr const char *FIRMWARE_VERSION = "2.7.0.cardputeros"; // what the apps compare against their minimum
constexpr uint32_t PORT_ADMIN = 6;

// settings the phone may change (AdminMessage to the local node); the app applies and saves them
struct AdminHooks {
    void (*set_owner)(const User &u) = nullptr;
    void (*set_lora)(uint32_t region, uint32_t preset, uint32_t hop_limit, int32_t tx_power, uint32_t slot, bool ok_to_mqtt) = nullptr;
    void (*set_channel)(const char *name, const uint8_t *psk, size_t psk_len) = nullptr;
    void (*set_time)(uint32_t unix_time) = nullptr;
};

// everything the phone API reports about this node; the app fills it
struct NodeSettings {
    uint32_t region_code = 6, preset_code = 0, hop_limit = 3, slot = 0;
    int32_t tx_power = 22;
    bool ok_to_mqtt = true;
    const uint8_t *psk = nullptr;
    size_t psk_len = 0;
    const char *channel_name = "";   // "" = default channel
    uint32_t (*now_unix)(void) = nullptr;
    uint32_t (*now_ms)(void) = nullptr;
};

inline void pb_msg(PbWriter &w, uint32_t field, const uint8_t *b, size_t n) {
    w.tag(field, 2);
    w.varint(n);
    for (size_t i = 0; i < n; i++) w.byte(b[i]);
}

// MeshPacket as the phone sees it: decoded, channel index 0
inline size_t encode_phone_packet(const Header &h, const Data &d, uint32_t rx_time, int16_t rssi, float snr, uint8_t *out,
                                  size_t cap) {
    uint8_t db[MAX_PAYLOAD + 32];
    size_t dn = const_cast<Data &>(d).encode(db, sizeof(db));
    PbWriter m(out, cap);
    m.fixed32(1, h.from);
    m.fixed32(2, h.to);
    pb_msg(m, 4, db, dn);
    m.fixed32(6, h.id);
    m.fixed32(7, rx_time);
    m.flt(8, snr);
    m.u32(9, h.hop_limit());
    m.boolean(10, h.want_ack());
    m.i32(12, rssi);
    m.boolean(14, h.via_mqtt());
    m.u32(15, h.hop_start());
    return m.ok ? m.len : 0;
}

struct PhonePacket {          // a MeshPacket the phone asked us to send
    uint32_t to = BROADCAST, id = 0, hop_limit = 0, channel = 0;
    bool want_ack = false;
    Data data;
    bool ok = false;
};

inline PhonePacket decode_phone_packet(const uint8_t *b, size_t n) {
    PhonePacket p;
    PbReader r(b, n);
    while (uint32_t f = r.next()) {
        if (f == 2 && r.wt == 5) p.to = r.fixed32();
        else if (f == 3 && r.wt == 0) p.channel = (uint32_t)r.varint();
        else if (f == 4 && r.wt == 2) {
            const uint8_t *db;
            size_t dl;
            if (r.bytes(db, dl)) p.ok = p.data.decode(db, dl);
        } else if (f == 6 && r.wt == 5) p.id = r.fixed32();
        else if (f == 9 && r.wt == 0) p.hop_limit = (uint32_t)r.varint();
        else if (f == 10 && r.wt == 0) p.want_ack = r.varint() != 0;
        else r.skip();
    }
    p.ok = p.ok && r.ok;
    return p;
}

class PhoneApi {
  public:
    Router *R = nullptr;
    NodeSettings S;
    AdminHooks admin;
    bool connected = false; // a client finished asking for the config

    void begin(Router *r) { R = r; }

    // ---- phone -> node ----
    void on_to_radio(const uint8_t *b, size_t n) {
        PbReader r(b, n);
        while (uint32_t f = r.next()) {
            if (f == 3 && r.wt == 0) start_config((uint32_t)r.varint());
            else if (f == 1 && r.wt == 2) {
                const uint8_t *pb;
                size_t pl;
                if (r.bytes(pb, pl)) handle_packet(pb, pl);
            } else if (f == 4 && r.wt == 0) { // disconnect
                r.varint();
                reset();
            } else r.skip();
        }
    }

    void reset() {
        state_ = IDLE;
        connected = false;
        qn_ = qh_ = 0;
    }

    // ---- node -> phone ----
    // next FromRadio message; false when there is nothing to send right now
    bool next(uint8_t *out, size_t cap, size_t &n) {
        n = 0;
        while (state_ != IDLE && state_ != LIVE) {
            n = config_step(out, cap);
            if (n) return true;
        }
        if (state_ == LIVE && qn_) {
            Item &it = q_[qh_];
            n = it.len <= cap ? it.len : 0;
            memcpy(out, it.b, n);
            qh_ = (qh_ + 1) % QN;
            qn_--;
            return n > 0;
        }
        return false;
    }
    bool pending() const { return (state_ != IDLE && state_ != LIVE) || (state_ == LIVE && qn_); }

    // a decoded mesh packet for us or broadcast (Listener::on_packet)
    void on_mesh_packet(const Header &h, const Data &d, int16_t rssi, float snr) {
        if (state_ != LIVE) return;
        uint8_t mp[MAX_FROM_RADIO];
        size_t mn = encode_phone_packet(h, d, unix_now(), rssi, snr, mp, sizeof(mp));
        if (mn) push_packet(mp, mn);
    }

    // our router's verdict on a packet the phone sent: tell the phone as a Routing message (implicit acks of
    // broadcasts and failures; a direct message's real ack arrives as a packet)
    void on_ack(uint32_t id, bool ok, uint32_t error) {
        if (state_ != LIVE) return;
        uint32_t to = 0;
        if (!take_sent(id, to)) return;
        if (ok && to != BROADCAST) return; // the destination's own Routing ack already went to the phone (on_packet)
        Data d;
        d.portnum = PORT_ROUTING;
        d.payload_len = encode_routing(ok ? ROUTING_NONE : (error ? error : ROUTING_MAX_RETRANSMIT), d.payload, sizeof(d.payload));
        d.request_id = id;
        Header h;
        h.from = (ok && to != BROADCAST) ? to : R->me; // an implicit ack comes from us, like the firmware's
        h.to = R->me;
        h.id = ++ack_id_;
        uint8_t mp[MAX_FROM_RADIO];
        size_t mn = encode_phone_packet(h, d, unix_now(), 0, 0, mp, sizeof(mp));
        if (mn) push_packet(mp, mn);
    }

  private:
    enum State { IDLE, MY_INFO, OWN_NODE, METADATA, CHANNELS, CONFIG, MODULES, NODES, COMPLETE, LIVE };
    State state_ = IDLE;
    uint32_t nonce_ = 0, step_ = 0, from_id_ = 0, ack_id_ = 0x7F000000;

    static constexpr int QN = 8;
    struct Item { size_t len; uint8_t b[MAX_FROM_RADIO]; };
    Item q_[QN];
    int qh_ = 0, qn_ = 0;

    static constexpr int SENT = 8;
    struct Sent { uint32_t id, to; };
    Sent sent_[SENT] = {};
    int sent_pos_ = 0;

    uint32_t unix_now() const { return S.now_unix ? S.now_unix() : 0; }

    void start_config(uint32_t nonce) {
        nonce_ = nonce;
        step_ = 0;
        qn_ = qh_ = 0;
        connected = false;
        state_ = nonce == NONCE_ONLY_NODES ? OWN_NODE : MY_INFO;
    }

    void remember_sent(uint32_t id, uint32_t to) {
        sent_[sent_pos_] = {id, to};
        sent_pos_ = (sent_pos_ + 1) % SENT;
    }
    bool take_sent(uint32_t id, uint32_t &to) {
        for (auto &s : sent_)
            if (s.id == id && id) { to = s.to; s.id = 0; return true; }
        return false;
    }

    void handle_packet(const uint8_t *b, size_t n) {
        PhonePacket p = decode_phone_packet(b, n);
        if (!p.ok || !R) return;
        uint32_t id = p.id;
        if (p.data.portnum == PORT_ADMIN && (p.to == R->me || p.to == 0)) {
            handle_admin(p.data.payload, p.data.payload_len, id);
            queue_status(id, 0);
            return;
        }
        if (p.to == R->me || p.data.portnum == PORT_ADMIN) { // admin for other nodes is not supported
            queue_status(id, 0);
            return;
        }
        uint8_t saved = R->hop_limit;
        if (p.hop_limit && p.hop_limit <= 7) R->hop_limit = (uint8_t)p.hop_limit;
        bool want_ack = p.want_ack || p.to != BROADCAST;
        uint32_t sent = R->send_data(p.to, p.data, want_ack && p.to != BROADCAST, 0, id);
        R->hop_limit = saved;
        if (sent) remember_sent(sent, p.to);
        queue_status(sent ? sent : id, sent ? 0 : 1);
    }

    // ---- framing of FromRadio ----
    size_t wrap(uint32_t field, const uint8_t *b, size_t n, uint8_t *out, size_t cap) {
        PbWriter w(out, cap);
        w.u32(1, ++from_id_);
        pb_msg(w, field, b, n);
        return w.ok ? w.len : 0;
    }

    void push_item(const uint8_t *b, size_t n) {
        if (!n) return;
        if (qn_ == QN) { qh_ = (qh_ + 1) % QN; qn_--; } // phone not reading: drop the oldest
        Item &it = q_[(qh_ + qn_) % QN];
        it.len = n;
        memcpy(it.b, b, n);
        qn_++;
    }
    void push_packet(const uint8_t *mp, size_t mn) {
        uint8_t fr[MAX_FROM_RADIO];
        push_item(fr, wrap(2, mp, mn, fr, sizeof(fr)));
    }
    void queue_status(uint32_t id, int32_t res) {
        uint8_t qs[24], fr[40];
        PbWriter w(qs, sizeof(qs));
        w.i32(1, res);
        w.u32(2, QN - qn_);
        w.u32(3, QN);
        w.u32(4, id);
        push_item(fr, wrap(11, qs, w.len, fr, sizeof(fr)));
    }

    size_t node_info(const Node &n, bool self, uint8_t *out, size_t cap) {
        uint8_t ub[128], pb[64], mb[48];
        PbWriter w(out, cap);
        w.u32(1, n.num);
        if (n.has_user) {
            User u = n.user;
            if (!u.id[0]) snprintf(u.id, sizeof(u.id), "!%08x", (unsigned)n.num);
            pb_msg(w, 2, ub, u.encode(ub, sizeof(ub)));
        }
        if (n.pos.has_pos) pb_msg(w, 3, pb, n.pos.encode(pb, sizeof(pb)));
        w.flt(4, n.snr);
        uint32_t ago = (n.last_heard && S.now_ms) ? (S.now_ms() - n.last_heard) / 1000 : 0;
        uint32_t t = unix_now();
        if (self) w.fixed32(5, t);
        else if (t && n.last_heard && ago < t) w.fixed32(5, t - ago);
        if (n.metrics.has) {
            PbWriter d(mb, sizeof(mb));
            d.u32(1, n.metrics.battery_level);
            d.flt(2, n.metrics.voltage);
            d.flt(3, n.metrics.channel_utilization);
            d.flt(4, n.metrics.air_util_tx);
            d.u32(5, n.metrics.uptime_seconds);
            pb_msg(w, 6, mb, d.len);
        }
        w.boolean(8, n.via_mqtt);
        if (!self && n.hops >= 0) w.u32(9, (uint32_t)n.hops);
        w.boolean(10, self); // the firmware marks itself favourite
        return w.ok ? w.len : 0;
    }

    // Config with only the section `which` (Config field number: 1 device ... 6 lora, 7 bluetooth, 8 security)
    void encode_config(uint32_t which, PbWriter &w) {
        uint8_t sb[64];
        PbWriter s(sb, sizeof(sb));
        if (which == 6) { // lora
            s.boolean(1, true);
            s.u32(2, S.preset_code);
            s.u32(7, S.region_code);
            s.u32(8, S.hop_limit);
            s.boolean(9, true);
            s.i32(10, S.tx_power);
            s.u32(11, S.slot);
            s.boolean(13, true);
            s.boolean(105, S.ok_to_mqtt);
        } else if (which == 7) { // bluetooth: on, no PIN
            s.boolean(1, true);
            s.u32(2, 2);
        }
        pb_msg(w, which, sb, s.len);
    }

    void encode_channel(uint32_t i, PbWriter &w) {
        if (i) w.u32(1, i); // index (0 is not written)
        if (i != 0) return; // secondary channels: disabled
        uint8_t sb[96], ms[8];
        PbWriter cs(sb, sizeof(sb));
        cs.bytes(2, S.psk, S.psk_len);
        cs.str(3, S.channel_name);
        PbWriter m(ms, sizeof(ms));
        m.u32(1, MAX_PUBLIC_PRECISION);
        pb_msg(cs, 7, ms, m.len);
        pb_msg(w, 2, sb, cs.len);
        w.u32(3, 1); // PRIMARY
    }

    void encode_metadata(PbWriter &w) {
        w.str(1, FIRMWARE_VERSION);
        w.u32(2, 24);       // device_state_version
        w.boolean(5, true); // hasBluetooth
        w.u32(9, HW_M5STACK_CARDPUTER_ADV);
    }

    // ---- AdminMessage to the local node ----
    void admin_reply(uint32_t request_id, uint32_t field, const uint8_t *b, size_t n) {
        uint8_t am[MAX_PAYLOAD];
        PbWriter a(am, sizeof(am));
        pb_msg(a, field, b, n);
        Data d;
        d.portnum = PORT_ADMIN;
        memcpy(d.payload, am, a.len);
        d.payload_len = a.len;
        d.request_id = request_id;
        Header h;
        h.from = R->me;
        h.to = R->me;
        h.id = ++ack_id_;
        uint8_t mp[MAX_FROM_RADIO];
        size_t mn = encode_phone_packet(h, d, unix_now(), 0, 0, mp, sizeof(mp));
        if (mn) push_packet(mp, mn);
    }

    void handle_admin(const uint8_t *b, size_t n, uint32_t id) {
        PbReader r(b, n);
        uint8_t out[MAX_PAYLOAD];
        while (uint32_t f = r.next()) {
            const uint8_t *m = nullptr;
            size_t ml = 0;
            if (r.wt == 2) r.bytes(m, ml);
            if (f == 32 && m) { // set_owner
                User u;
                if (u.decode(m, ml) && admin.set_owner) admin.set_owner(u);
            } else if (f == 33 && m) { // set_channel
                PbReader c(m, ml);
                uint32_t index = 0;
                const uint8_t *st = nullptr;
                size_t sl = 0;
                while (uint32_t g = c.next()) {
                    if (g == 1 && c.wt == 0) index = (uint32_t)c.varint();
                    else if (g == 2 && c.wt == 2) c.bytes(st, sl);
                    else c.skip();
                }
                if (index == 0 && st && admin.set_channel) {
                    PbReader cs(st, sl);
                    char name[16] = {0};
                    uint8_t psk[32];
                    size_t pl = 0;
                    while (uint32_t g = cs.next()) {
                        if (g == 2 && cs.wt == 2) {
                            const uint8_t *pp;
                            size_t pn;
                            if (cs.bytes(pp, pn) && pn <= 32) { memcpy(psk, pp, pn); pl = pn; }
                        } else if (g == 3 && cs.wt == 2) cs.str(name, sizeof(name));
                        else cs.skip();
                    }
                    admin.set_channel(name, psk, pl);
                }
            } else if (f == 34 && m) { // set_config: only the LoRa section is applied
                PbReader c(m, ml);
                while (uint32_t g = c.next()) {
                    if (g == 6 && c.wt == 2) {
                        const uint8_t *lb;
                        size_t ln;
                        if (!c.bytes(lb, ln)) break;
                        PbReader l(lb, ln);
                        uint32_t region = S.region_code, preset = S.preset_code, hop = S.hop_limit, slot = 0;
                        int32_t power = S.tx_power;
                        bool okm = false;
                        while (uint32_t h = l.next()) {
                            if (h == 2 && l.wt == 0) preset = (uint32_t)l.varint();
                            else if (h == 7 && l.wt == 0) region = (uint32_t)l.varint();
                            else if (h == 8 && l.wt == 0) hop = (uint32_t)l.varint();
                            else if (h == 10 && l.wt == 0) power = (int32_t)l.varint();
                            else if (h == 11 && l.wt == 0) slot = (uint32_t)l.varint();
                            else if (h == 105 && l.wt == 0) okm = l.varint() != 0;
                            else l.skip();
                        }
                        if (admin.set_lora) admin.set_lora(region, preset, hop, power, slot, okm);
                    } else c.skip();
                }
            } else if (f == 43 && r.wt == 5) { // set_time_only
                uint32_t t = r.fixed32();
                if (admin.set_time) admin.set_time(t);
            } else if (f == 3 && r.wt == 0) { // get_owner_request
                r.varint();
                admin_reply(id, 4, out, R->owner.encode(out, sizeof(out)));
            } else if (f == 5 && r.wt == 0) { // get_config_request (ConfigType: 0 device .. 5 lora, 6 bluetooth)
                uint32_t type = (uint32_t)r.varint();
                PbWriter w(out, sizeof(out));
                encode_config(type + 1, w);
                admin_reply(id, 6, out, w.len);
            } else if (f == 1 && r.wt == 0) { // get_channel_request (index + 1)
                uint32_t idx = (uint32_t)r.varint();
                PbWriter w(out, sizeof(out));
                encode_channel(idx ? idx - 1 : 0, w);
                admin_reply(id, 2, out, w.len);
            } else if (f == 12 && r.wt == 0) { // get_device_metadata_request
                r.varint();
                PbWriter w(out, sizeof(out));
                encode_metadata(w);
                admin_reply(id, 13, out, w.len);
            } else if (f == 7 && r.wt == 0) { // get_module_config_request: defaults
                uint32_t type = (uint32_t)r.varint();
                PbWriter w(out, sizeof(out));
                pb_msg(w, type + 1, out, 0);
                admin_reply(id, 8, out, w.len);
            } else if (!m && r.wt != 2) {
                r.skip();
            }
        }
    }

    // one FromRadio of the config sequence; 0 = this step produced nothing (advance and try again)
    size_t config_step(uint8_t *out, size_t cap) {
        uint8_t b[MAX_FROM_RADIO];
        switch (state_) {
        case MY_INFO: {
            PbWriter w(b, sizeof(b));
            w.u32(1, R->me);
            w.str(13, "m5stack-cardputer-adv-cardputeros");
            w.u32(15, (uint32_t)R->num_nodes + 1);
            state_ = OWN_NODE;
            return wrap(3, b, w.len, out, cap);
        }
        case OWN_NODE: {
            Node self;
            self.num = R->me;
            self.user = R->owner;
            self.has_user = true;
            if (own_pos) self.pos = *own_pos;
            if (own_metrics) self.metrics = *own_metrics;
            state_ = nonce_ == NONCE_ONLY_NODES ? NODES : METADATA;
            step_ = 0;
            return wrap(4, b, node_info(self, true, b, sizeof(b)), out, cap);
        }
        case METADATA: {
            PbWriter w(b, sizeof(b));
            encode_metadata(w);
            state_ = CHANNELS;
            step_ = 0;
            return wrap(13, b, w.len, out, cap);
        }
        case CHANNELS: {
            PbWriter w(b, sizeof(b));
            encode_channel(step_++, w);
            if (step_ >= 8) { state_ = CONFIG; step_ = 1; }
            return wrap(10, b, w.len, out, cap);
        }
        case CONFIG: {
            uint32_t which = step_++;
            PbWriter w(b, sizeof(b));
            encode_config(which, w);
            if (step_ > 8) { state_ = MODULES; step_ = 1; }
            return wrap(5, b, w.len, out, cap);
        }
        case MODULES: {
            uint32_t which = step_++;
            PbWriter w(b, sizeof(b));
            pb_msg(w, which, b, 0); // every module present, defaults
            if (step_ > 13) {
                state_ = nonce_ == NONCE_ONLY_CONFIG ? COMPLETE : NODES;
                step_ = 0;
            }
            return wrap(9, b, w.len, out, cap);
        }
        case NODES: {
            while (step_ < (uint32_t)R->num_nodes) {
                const Node &n = R->nodes[step_++];
                if (n.num == R->me) continue;
                return wrap(4, b, node_info(n, false, b, sizeof(b)), out, cap);
            }
            state_ = COMPLETE;
            return 0;
        }
        case COMPLETE: {
            PbWriter w(out, cap);
            w.u32(1, ++from_id_);
            w.tag(7, 0);
            w.varint(nonce_);
            state_ = LIVE;
            connected = true;
            return w.ok ? w.len : 0;
        }
        default:
            state_ = LIVE;
            return 0;
        }
    }

  public:
    const Position *own_pos = nullptr;       // optional: our position for our own NodeInfo
    const DeviceMetrics *own_metrics = nullptr;
};

} // namespace phone
