// Host harness for the phone API: serves it on TCP 4403 with the Meshtastic stream framing
// (0x94 0xC3, 16-bit length), as a node with WiFi does. Two routers share an in-memory "air":
// A is the node the phone talks to, B a remote node that announces itself and answers "ping"
// with "pong". test/phone_test.py connects with the official Python client.
//   g++ -std=c++17 -Iapps/meshtastic/src apps/meshtastic/test/phone_server.cpp -lcrypto -o /tmp/ps
#include "phone_api.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
using namespace mesh;

static int aes(const uint8_t *k, int bits, const uint8_t in[16], uint8_t out[16]) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int l;
    EVP_EncryptInit_ex(c, bits == 256 ? EVP_aes_256_ecb() : EVP_aes_128_ecb(), 0, k, 0);
    EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_EncryptUpdate(c, out, &l, in, 16);
    EVP_CIPHER_CTX_free(c);
    return 0;
}
static uint32_t now_ms() { timeval t; gettimeofday(&t, 0); return (uint32_t)(t.tv_sec * 1000 + t.tv_usec / 1000); }
static uint32_t now_unix() { return (uint32_t)time(0); }
static uint32_t rnd() { static uint32_t s = 0x1234; s = s * 1103515245u + 12345u; return s ^ (s >> 15); }
static std::vector<std::vector<uint8_t>> airA, airB; // sent by A, sent by B
static int sendA(const uint8_t *p, size_t n) { airA.emplace_back(p, p + n); return 0; }
static int sendB(const uint8_t *p, size_t n) { airB.emplace_back(p, p + n); return 0; }
static int idle() { return 0; }

Router A, B;
phone::PhoneApi api;

struct ListA : Listener {
    void on_text(const Header &, const Data &, const Node *) override {}
    void on_ack(uint32_t id, bool ok, uint32_t err) override { api.on_ack(id, ok, err); }
    void on_node(const Node &) override {}
    void on_packet(const Header &h, const Data &d, int16_t rssi, float snr) override { api.on_mesh_packet(h, d, rssi, snr); }
} la;
struct ListB : Listener {
    void on_text(const Header &h, const Data &d, const Node *) override {
        fprintf(stderr, "B got text: %.*s\n", (int)d.payload_len, d.payload);
        if (d.payload_len == 4 && !memcmp(d.payload, "ping", 4)) B.send_text(h.to == BROADCAST ? BROADCAST : h.from, "pong");
    }
    void on_ack(uint32_t, bool, uint32_t) override {}
    void on_node(const Node &) override {}
} lb;

static void air() { // what each router sent reaches the other
    for (auto &p : airB) A.on_receive(p.data(), p.size(), -80, 7.5f);
    airB.clear();
    for (auto &p : airA) B.on_receive(p.data(), p.size(), -82, 6.0f);
    airA.clear();
}

static void set_owner(const User &u) {
    strcpy(A.owner.long_name, u.long_name);
    strcpy(A.owner.short_name, u.short_name);
    fprintf(stderr, "ADMIN owner %s / %s\n", u.long_name, u.short_name);
}
static void set_lora(uint32_t region, uint32_t preset, uint32_t hop, int32_t power, uint32_t slot, bool okm) {
    api.S.hop_limit = hop;
    A.hop_limit = (uint8_t)hop;
    fprintf(stderr, "ADMIN lora region %u preset %u hops %u power %d\n", region, preset, hop, power);
}
static void set_time(uint32_t t) { fprintf(stderr, "ADMIN time %u\n", t); }

int main() {
    uint8_t one = 1;
    A.me = 0xA1A2A3A4; B.me = 0xB1B2B3B4;
    strcpy(A.owner.id, "!a1a2a3a4"); strcpy(A.owner.long_name, "Cardputer AR"); strcpy(A.owner.short_name, "CPAR");
    A.owner.hw_model = HW_M5STACK_CARDPUTER_ADV;
    strcpy(B.owner.id, "!b1b2b3b4"); strcpy(B.owner.long_name, "Nodo Rosario"); strcpy(B.owner.short_name, "ROS");
    A.begin(Platform{now_ms, rnd, aes, sendA, idle}, &la);
    B.begin(Platform{now_ms, rnd, aes, sendB, idle}, &lb);
    A.set_channel("", &one, 1); B.set_channel("", &one, 1);
    api.begin(&A);
    api.S.psk = &one; api.S.psk_len = 1; api.S.now_unix = now_unix; api.S.now_ms = now_ms;
    api.admin.set_owner = set_owner;
    api.admin.set_lora = set_lora;
    api.admin.set_time = set_time;
    B.send_nodeinfo(); // so A knows B before the phone connects
    for (uint32_t t0 = now_ms(); now_ms() - t0 < 1500;) {
        A.loop(); B.loop();
        air();
        usleep(5000);
    }
    printf("A knows %d nodes\n", A.num_nodes);

    int ls = socket(AF_INET, SOCK_STREAM, 0), one_i = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one_i, sizeof(one_i));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(4403); a.sin_addr.s_addr = htonl(0x7f000001);
    if (bind(ls, (sockaddr *)&a, sizeof a) || listen(ls, 1)) { perror("bind"); return 1; }
    printf("listening\n"); fflush(stdout);
    int c = accept(ls, 0, 0);
    fcntl(c, F_SETFL, O_NONBLOCK);
    std::vector<uint8_t> in;
    for (uint32_t t0 = now_ms(); now_ms() - t0 < 30000;) {
        uint8_t buf[1024];
        int r = read(c, buf, sizeof buf);
        if (r == 0) break;
        if (r > 0) in.insert(in.end(), buf, buf + r);
        // frames: 0x94 0xC3 len_hi len_lo payload
        while (in.size() >= 4) {
            if (in[0] != 0x94 || in[1] != 0xC3) { in.erase(in.begin()); continue; }
            size_t n = in[2] << 8 | in[3];
            if (in.size() < 4 + n) break;
            api.on_to_radio(in.data() + 4, n);
            in.erase(in.begin(), in.begin() + 4 + n);
        }
        A.loop(); B.loop();
        air();
        uint8_t fr[phone::MAX_FROM_RADIO];
        size_t n;
        while (api.next(fr, sizeof fr, n)) {
            uint8_t hdr[4] = {0x94, 0xC3, (uint8_t)(n >> 8), (uint8_t)n};
            write(c, hdr, 4);
            write(c, fr, n);
        }
        usleep(3000);
    }
    close(c);
    return 0;
}
