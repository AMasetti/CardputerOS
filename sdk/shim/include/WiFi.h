#pragma once
#include "Arduino.h"

enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };
enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK };
enum { WL_NO_SHIELD = 255, WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_SCAN_COMPLETED = 2, WL_CONNECTED = 3,
       WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6 };
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

class IPAddress {
public:
    IPAddress() : a_(0) {}
    IPAddress(uint32_t a) : a_(a) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { a_ = (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24); }
    operator uint32_t() const { return a_; }
    uint8_t operator[](int i) const { return (a_ >> (8 * i)) & 0xFF; }
    bool operator==(const IPAddress &o) const { return a_ == o.a_; }
    bool operator!=(const IPAddress &o) const { return a_ != o.a_; }
    String toString() const {
        char b[20];
        snprintf(b, sizeof(b), "%u.%u.%u.%u", (unsigned)(*this)[0], (unsigned)(*this)[1], (unsigned)(*this)[2], (unsigned)(*this)[3]);
        return String(b);
    }
    uint32_t raw() const { return a_; }
    bool fromString(const char *s) {
        unsigned v[4];
        char tail;
        if (sscanf(s, "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4) return false;
        for (int i = 0; i < 4; i++) if (v[i] > 255) return false;
        *this = IPAddress(v[0], v[1], v[2], v[3]);
        return true;
    }
    bool fromString(const String &s) { return fromString(s.c_str()); }

private:
    uint32_t a_;  // network byte order, as lwip stores it
};

class WiFiClass {
public:
    bool mode(int m) { return cpos_api->net.mode(m == WIFI_STA || m == WIFI_AP_STA) == 0; }
    int begin(const char *ssid, const char *pass = nullptr) { cpos_api->net.connect(ssid, pass ? pass : ""); return status(); }
    bool disconnect(bool = false, bool = false) { cpos_api->net.disconnect(); return true; }
    int status() { return cpos_api->net.status(); }
    bool isConnected() { return status() == WL_CONNECTED; }
    IPAddress localIP() { return IPAddress(cpos_api->net.ip()); }
    IPAddress broadcastIP() { return IPAddress(cpos_api->net.broadcast_ip()); }
    int32_t RSSI() { return cpos_api->net.rssi(); }
    String SSID() { return String(cpos_api->net.ssid()); }
    void setSleep(bool on) { cpos_api->net.set_sleep(on); }
    void setAutoReconnect(bool) {}
    int16_t scanNetworks(bool async = false, bool = false) { return cpos_api->net.scan_start(async); }
    int16_t scanComplete() { return cpos_api->net.scan_count(); }
    void scanDelete() { cpos_api->net.scan_clear(); }
    String SSID(uint8_t i) { cpos_ap_t a; return cpos_api->net.scan_get(i, &a) == 0 ? String(a.ssid) : String(); }
    int32_t RSSI(uint8_t i) { cpos_ap_t a; return cpos_api->net.scan_get(i, &a) == 0 ? a.rssi : 0; }
    int encryptionType(uint8_t i) { cpos_ap_t a; return cpos_api->net.scan_get(i, &a) == 0 ? (a.auth_open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK) : WIFI_AUTH_OPEN; }
    int32_t channel(uint8_t i) { cpos_ap_t a; return cpos_api->net.scan_get(i, &a) == 0 ? a.channel : 0; }
};
extern WiFiClass WiFi;
