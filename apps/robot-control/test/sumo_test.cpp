// Host harness: drives sumo::Link against the fake drone in run_host_test.py over real sockets.
// usage: host_test <disc_port> <d2c_port> <out_dir>
#include "../src/sumo_core.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <string>

static uint64_t now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000ull + t.tv_nsec / 1000000; }

struct L : sumo::Listener {
    int udp = -1;
    sockaddr_in drone{};
    std::string dir;
    int jpegs = 0, battery = -1, posture = -1, alert = -1;
    void send(const uint8_t *d, size_t n) override { sendto(udp, d, n, 0, (sockaddr *)&drone, sizeof(drone)); }
    void on_jpeg(const uint8_t *d, size_t n) override {
        char name[256];
        snprintf(name, sizeof(name), "%s/frame_%d.jpg", dir.c_str(), jpegs++);
        FILE *f = fopen(name, "wb");
        if (f) { fwrite(d, 1, n, f); fclose(f); }
    }
    void on_battery(int p) override { battery = p; }
    void on_posture(int p) override { posture = p; }
    void on_alert(int a) override { alert = a; }
};

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    int disc = atoi(argv[1]), d2c = atoi(argv[2]);
    L l;
    l.dir = argv[3];

    // discovery over TCP
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(disc);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(tcp, (sockaddr *)&a, sizeof(a))) { perror("connect"); return 3; }
    char hello[160];
    snprintf(hello, sizeof(hello), "{\"controller_type\":\"cardputer\",\"controller_name\":\"cardputeros\",\"d2c_port\":\"%d\"}", d2c);
    send(tcp, hello, strlen(hello) + 1, 0);
    char reply[512] = {0};
    recv(tcp, reply, sizeof(reply) - 1, 0);
    close(tcp);
    int c2d = sumo::Link::parse_c2d_port(reply, 54321);
    printf("c2d_port=%d\n", c2d);

    l.udp = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port = htons(d2c);
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(l.udp, (sockaddr *)&me, sizeof(me))) { perror("bind"); return 4; }
    fcntl(l.udp, F_SETFL, O_NONBLOCK);
    l.drone = a;
    l.drone.sin_port = htons(c2d);

    sumo::Link link(&l);
    link.send_all_states();
    link.send_video_enable(true);
    link.send_jump(sumo::JUMP_HIGH);
    link.send_animation(sumo::ANIM_SPIN);
    link.send_posture(sumo::POSTURE_JUMPER);
    uint64_t start = now_ms(), last = 0;
    static uint8_t buf[2048];
    while (now_ms() - start < 6000) {
        if (now_ms() - last >= 25) { link.send_pcmd(true, 50, -20); last = now_ms(); }
        ssize_t n;
        while ((n = recv(l.udp, buf, sizeof(buf), 0)) > 0) link.on_datagram(buf, n);
        usleep(1000);
    }
    link.send_pcmd(false, 0, 0);
    printf("jpegs=%d ok=%u dropped=%u battery=%d posture=%d alert=%d\n", l.jpegs, link.frames_ok, link.frames_dropped, l.battery, l.posture, l.alert);
    return 0;
}
