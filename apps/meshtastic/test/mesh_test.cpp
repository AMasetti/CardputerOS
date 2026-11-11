// Host test of the Meshtastic core (no hardware):
//   g++ -std=c++17 -Iapps/meshtastic/src apps/meshtastic/test/mesh_test.cpp -lcrypto -o /tmp/t && /tmp/t
// With --dump it also prints packets for test/crosscheck.py, which checks them against the
// official Meshtastic protobufs and an independent AES-CTR.
#include "mesh_core.h"
#include "mqtt_core.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

using namespace mesh;

static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } \
    } while (0)

static int aes_ecb(const uint8_t *key, int bits, const uint8_t in[16], uint8_t out[16]) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int l = 0;
    EVP_EncryptInit_ex(c, bits == 256 ? EVP_aes_256_ecb() : EVP_aes_128_ecb(), nullptr, key, nullptr);
    EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_EncryptUpdate(c, out, &l, in, 16);
    EVP_CIPHER_CTX_free(c);
    return 0;
}

// ---- a tiny simulated air: every node hears every other node ----
static uint32_t g_now = 1000;
static uint32_t g_seed = 12345;
static uint32_t now_fn() { return g_now; }
static uint32_t rnd_fn() { g_seed = g_seed * 1103515245u + 12345u; return g_seed ^ (g_seed >> 16); }

struct Air { std::vector<std::vector<uint8_t>> sent[3]; };
static Air air;
static int send0(const uint8_t *p, size_t n) { air.sent[0].emplace_back(p, p + n); return 0; }
static int send1(const uint8_t *p, size_t n) { air.sent[1].emplace_back(p, p + n); return 0; }
static int send2(const uint8_t *p, size_t n) { air.sent[2].emplace_back(p, p + n); return 0; }
static int not_busy() { return 0; }

struct Rec : Listener {
    std::vector<std::string> texts;
    std::vector<std::pair<uint32_t, bool>> acks;
    int nodes = 0;
    void on_text(const Header &, const Data &d, const Node *) override { texts.emplace_back((const char *)d.payload, d.payload_len); }
    void on_ack(uint32_t id, bool ok, uint32_t) override { acks.push_back({id, ok}); }
    void on_node(const Node &) override { nodes++; }
};

static void hex(const char *tag, const uint8_t *p, size_t n) {
    printf("%s ", tag);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    bool dump = argc > 1 && !strcmp(argv[1], "--dump");

    // --- channel and frequency: reference values of the firmware ---
    uint8_t key[32];
    uint8_t one = 1;
    size_t kl = expand_psk(&one, 1, key);
    CHECK(kl == 16 && !memcmp(key, DEFAULT_PSK, 16));
    CHECK(channel_hash("LongFast", key, kl) == 8); // the default channel's hash is 8
    uint8_t two = 2;
    expand_psk(&two, 1, key);
    CHECK(key[15] == 0x02);
    const Preset &lf = PRESETS[0];
    CHECK(num_slots(REGIONS[0], lf) == 52);
    CHECK(frequency_hz(REGIONS[0], lf, "LongFast", 0) == 919875000u); // ANZ LongFast
    CHECK(frequency_hz(REGIONS[1], lf, "LongFast", 0) == 906875000u); // US LongFast
    CHECK(frequency_hz(REGIONS[0], lf, "LongFast", 1) == 915125000u);
    CHECK(frequency_hz(REGIONS[0], PRESETS[5], "MediumFast", 0) == 915000000u + 125000u + (djb2("MediumFast") % 52) * 250000u);
    CHECK(slot_time_ms(lf) == 28);
    uint32_t at = airtime_ms(lf, 50);
    CHECK(at > 450 && at < 700); // LongFast 50 bytes ~ 0.6 s

    // --- position precision on public channels (firmware: 15 bits, centred) ---
    {
        CHECK(psk_is_public(&one, 1) && psk_is_public(DEFAULT_PSK, 16));
        uint8_t k16[16] = {9};
        CHECK(!psk_is_public(k16, 16));
        int32_t lat = -329442123, full = lat;
        reduce_precision(lat, 15);
        CHECK(lat != full && (lat > full ? lat - full : full - lat) < (1 << 17));
        int32_t again = lat;
        reduce_precision(again, 15);
        CHECK(again == lat); // stable
    }

    // --- MQTT framing ---
    {
        uint8_t b[300];
        size_t n = mqtt::connect("!0000cafe", mqtt::USER, mqtt::PASS, b, sizeof(b));
        CHECK(n == 2 + 10 + 11 + 9 + 12 && b[0] == 0x10 && b[1] == n - 2 && !memcmp(b + 4, "MQTT", 4) && b[9] == 0xC2);
        uint8_t pl[200] = {1, 2, 3};
        n = mqtt::publish("msh/ANZ/2/e/LongFast/!0000cafe", pl, 200, b, sizeof(b));
        CHECK(n == 1 + 2 + 2 + 30 + 200 && b[0] == 0x30 && b[1] == (0x80 | ((232) & 0x7F)) && b[2] == 1);
        uint8_t ack[4] = {0x20, 2, 0, 0};
        CHECK(mqtt::connack_ok(ack, 4));
        ack[3] = 5;
        CHECK(!mqtt::connack_ok(ack, 4));
    }

    // --- base64 ---
    char b64[64];
    b64_encode(&one, 1, b64, sizeof(b64));
    CHECK(!strcmp(b64, "AQ=="));
    uint8_t back[32];
    CHECK(b64_decode("AQ==", back, sizeof(back)) == 1 && back[0] == 1);
    b64_encode(DEFAULT_PSK, 16, b64, sizeof(b64));
    CHECK(b64_decode(b64, back, sizeof(back)) == 16 && !memcmp(back, DEFAULT_PSK, 16));

    // --- protobuf round trips ---
    {
        User u;
        strcpy(u.id, "!a1b2c3d4");
        strcpy(u.long_name, "Cardputer Ñandú");
        strcpy(u.short_name, "CPA");
        u.hw_model = HW_M5STACK_CARDPUTER_ADV;
        uint8_t b[128];
        size_t n = u.encode(b, sizeof(b));
        User v;
        CHECK(n && v.decode(b, n) && !strcmp(v.long_name, u.long_name) && v.hw_model == 112 && !strcmp(v.short_name, "CPA"));
        Position p;
        p.has_pos = true;
        p.latitude_i = -346037000; // Buenos Aires
        p.longitude_i = -583816000;
        p.altitude = 25;
        p.time = 1793000000;
        p.precision_bits = 32;
        n = p.encode(b, sizeof(b));
        Position q;
        CHECK(n && q.decode(b, n) && q.has_pos && q.latitude_i == p.latitude_i && q.longitude_i == p.longitude_i && q.altitude == 25);
        DeviceMetrics m;
        m.battery_level = 87;
        m.voltage = 4.01f;
        m.uptime_seconds = 3600;
        n = encode_telemetry(1793000000, m, b, sizeof(b));
        DeviceMetrics m2;
        CHECK(n && decode_telemetry(b, n, m2) && m2.battery_level == 87 && m2.uptime_seconds == 3600);
        uint32_t err = 9;
        n = encode_routing(ROUTING_NONE, b, sizeof(b));
        CHECK(n == 2 && decode_routing(b, n, err) && err == 0);
    }

    // --- three routers: A <-> B <-> C is simulated by delivering everything to everyone,
    //     but C only hears B (so A's packets reach C through B's relay) ---
    Rec ra, rb, rc;
    Router A, B, C;
    Platform pa{now_fn, rnd_fn, aes_ecb, send0, not_busy}, pb{now_fn, rnd_fn, aes_ecb, send1, not_busy},
        pc{now_fn, rnd_fn, aes_ecb, send2, not_busy};
    A.me = 0xA0000001; B.me = 0xB0000002; C.me = 0xC0000003;
    strcpy(A.owner.long_name, "Nodo A"); strcpy(A.owner.short_name, "A");
    strcpy(B.owner.long_name, "Nodo B"); strcpy(B.owner.short_name, "B");
    strcpy(C.owner.long_name, "Nodo C"); strcpy(C.owner.short_name, "C");
    A.begin(pa, &ra); B.begin(pb, &rb); C.begin(pc, &rc);
    A.set_channel("", &one, 1); B.set_channel("", &one, 1); C.set_channel("", &one, 1);
    CHECK(!strcmp(A.channel_name(), "LongFast"));

    // links: A-B, B-C (A and C cannot hear each other)
    auto hears = [](int from, int to) { return (from == 0 && to == 1) || (from == 1 && to == 0) || (from == 1 && to == 2) || (from == 2 && to == 1); };
    Router *R[3] = {&A, &B, &C};
    auto run = [&](uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            g_now += 10;
            for (int i = 0; i < 3; i++) R[i]->loop();
            for (int i = 0; i < 3; i++) {
                auto pk = air.sent[i];
                air.sent[i].clear();
                for (auto &p : pk)
                    for (int j = 0; j < 3; j++)
                        if (hears(i, j)) R[j]->on_receive(p.data(), p.size(), -90, 5.0f);
            }
        }
    };

    // broadcast from A reaches C through B; A gets an implicit ack from B's relay
    uint32_t id = A.send_text(BROADCAST, "hola mesh");
    CHECK(id != 0);
    if (dump) hex("KEY", DEFAULT_PSK, 16);
    run(5000);
    CHECK(rb.texts.size() == 1 && rb.texts[0] == "hola mesh");
    CHECK(rc.texts.size() == 1 && rc.texts[0] == "hola mesh");
    CHECK(ra.texts.empty());
    bool implicit = false;
    for (auto &a : ra.acks) if (a.first == id && a.second) implicit = true;
    CHECK(implicit);
    CHECK(B.stats.relayed >= 1);
    Node *ca = C.find(A.me);
    CHECK(ca && ca->hops == 1); // one relay between them

    // direct message A -> C: relayed by B, C acks, the ack comes back through B
    ra.acks.clear();
    id = A.send_text(C.me, "privado");
    run(20000);
    CHECK(rc.texts.size() == 2 && rc.texts[1] == "privado");
    bool acked = false;
    for (auto &a : ra.acks) if (a.first == id && a.second) acked = true;
    CHECK(acked);

    // C learned nothing about A's name yet, so it asked; after the exchange it knows A
    run(40000);
    ca = C.find(A.me);
    CHECK(ca && ca->has_user && !strcmp(ca->user.long_name, "Nodo A"));

    // DM to a node that does not exist: retries, then fails
    ra.acks.clear();
    id = A.send_text(0x12345678, "nadie");
    run(120000);
    bool failed = false;
    for (auto &a : ra.acks) if (a.first == id && !a.second) failed = true;
    CHECK(failed);

    // another channel: B cannot read it, but still relays it (C on that channel reads it)
    uint8_t psk16[16];
    for (int i = 0; i < 16; i++) psk16[i] = (uint8_t)(i * 7 + 3);
    A.set_channel("Rosario", psk16, 16);
    C.set_channel("Rosario", psk16, 16);
    size_t before_b = rb.texts.size(), before_c = rc.texts.size();
    A.send_text(BROADCAST, "canal privado");
    run(5000);
    CHECK(rb.texts.size() == before_b);
    CHECK(rc.texts.size() == before_c + 1 && rc.texts.back() == "canal privado");

    // duplicate suppression: the same raw packet twice is delivered once
    {
        Rec rx;
        Router X;
        Platform px{now_fn, rnd_fn, aes_ecb, send0, not_busy};
        X.me = 0x0000BEEF;
        X.begin(px, &rx);
        X.set_channel("", &one, 1);
        Router Y;
        Rec ry;
        Platform py{now_fn, rnd_fn, aes_ecb, send1, not_busy};
        Y.me = 0x0000CAFE;
        Y.begin(py, &ry);
        Y.set_channel("", &one, 1);
        air.sent[1].clear();
        Y.send_text(BROADCAST, "dup");
        for (int i = 0; i < 50 && air.sent[1].empty(); i++) { g_now += 10; Y.loop(); }
        CHECK(!air.sent[1].empty());
        auto p = air.sent[1][0];
        X.on_receive(p.data(), p.size(), -80, 3);
        X.on_receive(p.data(), p.size(), -80, 3);
        CHECK(rx.texts.size() == 1);
        if (dump) hex("PKT", p.data(), p.size());
        air.sent[1].clear();
        air.sent[0].clear();

        // packets for the cross check: text, nodeinfo, position, telemetry
        if (dump) {
            strcpy(Y.owner.id, "!0000cafe");
            strcpy(Y.owner.long_name, "Cardputer AR");
            strcpy(Y.owner.short_name, "CPAR");
            Y.owner.hw_model = HW_M5STACK_CARDPUTER_ADV;
            Y.send_nodeinfo();
            Position pos;
            pos.has_pos = true;
            pos.latitude_i = -329442000; // Rosario
            pos.longitude_i = -606502000;
            pos.altitude = 31;
            pos.time = 1793000000;
            pos.location_source = 2;
            pos.precision_bits = 32;
            Y.send_position(pos);
            DeviceMetrics m;
            m.battery_level = 80;
            m.voltage = 3.95f;
            m.uptime_seconds = 120;
            Y.send_telemetry(1793000000, m);
            for (int i = 0; i < 3000; i++) { g_now += 10; Y.loop(); }
            for (auto &q : air.sent[1]) hex("PKT", q.data(), q.size());
            // what the MQTT uplink publishes: the first packet as an encrypted envelope, and a map report
            uint8_t env[400];
            size_t en = encode_envelope(air.sent[1][0].data(), air.sent[1][0].size(), 1793000000, -95, 6.5f, "LongFast",
                                        "!0000cafe", env, sizeof(env));
            hex("ENV", env, en);
            MapReport mr{"Cardputer AR", "CPAR", HW_M5STACK_CARDPUTER_ADV, REGIONS[0].code, PRESETS[0].code, "cardputeros-0.2.0",
                         true, -329442000, -606502000, 31, 4};
            en = encode_map_report(0x0000CAFE, mr, "LongFast", "!0000cafe", env, sizeof(env));
            hex("MAP", env, en);
        }
    }

    if (!dump) printf(fails ? "%d FAILED\n" : "all mesh tests passed\n", fails);
    return fails ? 1 : 0;
}
