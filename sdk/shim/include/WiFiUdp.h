#pragma once
#include "WiFi.h"

struct cpos_sockaddr_in {
    uint8_t len, family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};

class WiFiUDP {
public:
    ~WiFiUDP() { stop(); }
    uint8_t begin(uint16_t port) {
        stop();
        fd_ = cpos_api->net.socket(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0);
        if (fd_ < 0) return 0;
        int one = 1;
        cpos_api->net.setsockopt(fd_, 0xfff /*SOL_SOCKET*/, 0x0004 /*SO_REUSEADDR*/, &one, sizeof(one));
        cpos_api->net.setsockopt(fd_, 0xfff, 0x0020 /*SO_BROADCAST*/, &one, sizeof(one));
        cpos_sockaddr_in a = {sizeof(a), 2, htons_(port), 0, {0}};
        if (cpos_api->net.bind(fd_, &a, sizeof(a)) != 0) { stop(); return 0; }
        cpos_api->net.fcntl(fd_, 4 /*F_SETFL*/, 1 /*O_NONBLOCK*/);
        return 1;
    }
    void stop() { if (fd_ >= 0) { cpos_api->net.close(fd_); fd_ = -1; } }
    int beginPacket(IPAddress ip, uint16_t port) {
        if (fd_ < 0) { begin(0); }
        dst_ = ip;
        dport_ = port;
        txlen_ = 0;
        return fd_ >= 0;
    }
    size_t write(const uint8_t *b, size_t n) {
        if (txlen_ + n > sizeof(tx_)) n = sizeof(tx_) - txlen_;
        memcpy(tx_ + txlen_, b, n);
        txlen_ += n;
        return n;
    }
    size_t write(uint8_t c) { return write(&c, 1); }
    int endPacket() {
        cpos_sockaddr_in a = {sizeof(a), 2, htons_(dport_), dst_.raw(), {0}};
        int r = cpos_api->net.sendto(fd_, tx_, txlen_, 0, &a, sizeof(a));
        txlen_ = 0;
        return r >= 0;
    }
    int parsePacket() {
        if (fd_ < 0) return 0;
        cpos_sockaddr_in a;
        uint32_t al = sizeof(a);
        int n = cpos_api->net.recvfrom(fd_, rx_, sizeof(rx_), 0, &a, &al);
        if (n <= 0) { rxlen_ = rxpos_ = 0; return 0; }
        rxlen_ = n;
        rxpos_ = 0;
        from_ = IPAddress(a.addr);
        fport_ = htons_(a.port);
        return n;
    }
    int available() { return rxlen_ - rxpos_; }
    int read(uint8_t *b, size_t n) {
        int m = rxlen_ - rxpos_;
        if ((int)n < m) m = n;
        memcpy(b, rx_ + rxpos_, m);
        rxpos_ += m;
        return m;
    }
    int read(char *b, size_t n) { return read((uint8_t *)b, n); }
    int read() { return rxpos_ < rxlen_ ? rx_[rxpos_++] : -1; }
    IPAddress remoteIP() { return from_; }
    uint16_t remotePort() { return fport_; }

private:
    static uint16_t htons_(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
    int fd_ = -1;
    IPAddress dst_, from_;
    uint16_t dport_ = 0, fport_ = 0;
    uint8_t tx_[512], rx_[512];
    int txlen_ = 0, rxlen_ = 0, rxpos_ = 0;
};
