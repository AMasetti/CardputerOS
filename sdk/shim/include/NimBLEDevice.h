// NimBLE-Arduino (1.4 API) on top of the CardputerOS BLE service: observer + central, one connection.
// Scan results and notifications are delivered to the app's callbacks from the app's own task,
// between loop() calls and inside the blocking calls below, never from another task.
#pragma once
#include <Arduino.h>
#include <cpos/abi.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

extern void (*cpos_loop_hook)(void);

typedef enum {
    ESP_PWR_LVL_N24 = 0, ESP_PWR_LVL_N21, ESP_PWR_LVL_N18, ESP_PWR_LVL_N15, ESP_PWR_LVL_N12, ESP_PWR_LVL_N9,
    ESP_PWR_LVL_N6, ESP_PWR_LVL_N3, ESP_PWR_LVL_N0, ESP_PWR_LVL_P3, ESP_PWR_LVL_P6, ESP_PWR_LVL_P9,
    ESP_PWR_LVL_P12, ESP_PWR_LVL_P15, ESP_PWR_LVL_P18, ESP_PWR_LVL_P20,
} esp_power_level_t;

/* the NimBLE UUID types, enough for getNative()->u16.value */
enum { BLE_UUID_TYPE_16 = 16, BLE_UUID_TYPE_32 = 32, BLE_UUID_TYPE_128 = 128 };
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef struct { ble_uuid_t u; uint32_t value; } ble_uuid32_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; ble_uuid32_t u32; ble_uuid128_t u128; } ble_uuid_any_t;

#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_RANDOM 1

class NimBLEAddress {
public:
    NimBLEAddress() { memset(m_val, 0, 6); }
    NimBLEAddress(const uint8_t v[6], uint8_t type = BLE_ADDR_PUBLIC) : m_type(type) { memcpy(m_val, v, 6); }
    NimBLEAddress(const std::string &s, uint8_t type = BLE_ADDR_PUBLIC) : m_type(type) {
        unsigned b[6] = {0};
        sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]);
        for (int i = 0; i < 6; i++) m_val[i] = b[i];
    }
    bool operator==(const NimBLEAddress &o) const { return memcmp(m_val, o.m_val, 6) == 0; }
    bool operator!=(const NimBLEAddress &o) const { return !(*this == o); }
    bool equals(const NimBLEAddress &o) const { return *this == o; }
    std::string toString() const {
        char s[18];
        snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", m_val[5], m_val[4], m_val[3], m_val[2], m_val[1], m_val[0]);
        return s;
    }
    operator std::string() const { return toString(); }
    const uint8_t *getNative() const { return m_val; }
    uint8_t getType() const { return m_type; }

private:
    uint8_t m_val[6];
    uint8_t m_type = BLE_ADDR_PUBLIC;
};

class NimBLEUUID {
public:
    NimBLEUUID() { m_any.u.type = 0; }
    NimBLEUUID(uint16_t v) { m_any.u16.u.type = BLE_UUID_TYPE_16; m_any.u16.value = v; }
    NimBLEUUID(uint32_t v) { m_any.u32.u.type = BLE_UUID_TYPE_32; m_any.u32.value = v; }
    NimBLEUUID(const char *s) { parse(s); }
    NimBLEUUID(const std::string &s) { parse(s.c_str()); }
    /* from the OS form: 128-bit little endian + the size the peer used */
    NimBLEUUID(const uint8_t le[16], int bits) {
        if (bits == 16) { m_any.u16.u.type = BLE_UUID_TYPE_16; m_any.u16.value = le[12] | (le[13] << 8); }
        else if (bits == 32) { m_any.u32.u.type = BLE_UUID_TYPE_32; m_any.u32.value = le[12] | (le[13] << 8) | (le[14] << 16) | ((uint32_t)le[15] << 24); }
        else { m_any.u128.u.type = BLE_UUID_TYPE_128; memcpy(m_any.u128.value, le, 16); }
    }
    uint8_t bitSize() const { return m_any.u.type; }
    const ble_uuid_any_t *getNative() const { return &m_any; }
    /* everything compared in the 128-bit form, so 0x180F equals its full spelling */
    void to128(uint8_t out[16]) const {
        static const uint8_t base[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0, 0, 0, 0};
        memcpy(out, base, 16);
        if (m_any.u.type == BLE_UUID_TYPE_16) { out[12] = m_any.u16.value; out[13] = m_any.u16.value >> 8; }
        else if (m_any.u.type == BLE_UUID_TYPE_32) { for (int i = 0; i < 4; i++) out[12 + i] = m_any.u32.value >> (8 * i); }
        else if (m_any.u.type == BLE_UUID_TYPE_128) memcpy(out, m_any.u128.value, 16);
    }
    bool operator==(const NimBLEUUID &o) const {
        if (!m_any.u.type || !o.m_any.u.type) return m_any.u.type == o.m_any.u.type;
        uint8_t a[16], b[16];
        to128(a);
        o.to128(b);
        return memcmp(a, b, 16) == 0;
    }
    bool operator!=(const NimBLEUUID &o) const { return !(*this == o); }
    bool equals(const NimBLEUUID &o) const { return *this == o; }
    std::string toString() const {
        char s[40];
        if (m_any.u.type == BLE_UUID_TYPE_16) snprintf(s, sizeof(s), "0x%04x", m_any.u16.value);
        else if (m_any.u.type == BLE_UUID_TYPE_32) snprintf(s, sizeof(s), "0x%08lx", (unsigned long)m_any.u32.value);
        else {
            const uint8_t *v = m_any.u128.value;
            snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", v[15], v[14], v[13], v[12], v[11],
                     v[10], v[9], v[8], v[7], v[6], v[5], v[4], v[3], v[2], v[1], v[0]);
        }
        return s;
    }

private:
    void parse(const char *s) {
        uint8_t hex[16];
        int n = 0;
        for (const char *p = s; *p && n < 32; p++) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
            if (d < 0) continue;
            if (n % 2 == 0) hex[n / 2] = d << 4; else hex[n / 2] |= d;
            n++;
        }
        if (n == 4) { m_any.u16.u.type = BLE_UUID_TYPE_16; m_any.u16.value = (hex[0] << 8) | hex[1]; }
        else if (n == 8) { m_any.u32.u.type = BLE_UUID_TYPE_32; m_any.u32.value = ((uint32_t)hex[0] << 24) | (hex[1] << 16) | (hex[2] << 8) | hex[3]; }
        else if (n == 32) { m_any.u128.u.type = BLE_UUID_TYPE_128; for (int i = 0; i < 16; i++) m_any.u128.value[i] = hex[15 - i]; }
        else m_any.u.type = 0;
    }
    ble_uuid_any_t m_any;
};

/* ---- advertising -------------------------------------------------------------------- */
class NimBLEAdvertisedDevice {
public:
    NimBLEAddress getAddress() const { return m_addr; }
    int getRSSI() const { return m_rssi; }
    uint8_t getAdvType() const { return m_event; }
    size_t getPayloadLength() const { return m_payload.size(); }
    const uint8_t *getPayload() const { return m_payload.data(); }

    bool haveName() const { return find(0x09) || find(0x08); }
    std::string getName() const { return field(find(0x09) ? 0x09 : 0x08, 0); }
    bool haveManufacturerData() const { return find(0xFF); }
    std::string getManufacturerData() const { return field(0xFF, 0); }
    bool haveTXPower() const { return find(0x0A); }
    int8_t getTXPower() const { std::string s = field(0x0A, 0); return s.empty() ? 0 : (int8_t)s[0]; }
    bool haveAppearance() const { return find(0x19); }
    uint16_t getAppearance() const { std::string s = field(0x19, 0); return s.size() >= 2 ? (uint8_t)s[0] | ((uint8_t)s[1] << 8) : 0; }

    bool haveServiceUUID() const { return getServiceUUIDCount() > 0; }
    size_t getServiceUUIDCount() const { return services().size(); }
    NimBLEUUID getServiceUUID(size_t i = 0) const { auto v = services(); return i < v.size() ? v[i] : NimBLEUUID(); }
    bool isAdvertisingService(const NimBLEUUID &u) const { for (auto &s : services()) if (s == u) return true; return false; }

    bool haveServiceData() const { return getServiceDataCount() > 0; }
    size_t getServiceDataCount() const { return count(0x16) + count(0x20) + count(0x21); }
    NimBLEUUID getServiceDataUUID(size_t i = 0) const { std::string d = serviceData(i); return d.empty() ? NimBLEUUID() : m_sd_uuid; }
    std::string getServiceData(size_t i = 0) const { return serviceData(i); }

    std::string toString() const { return "Name: " + getName() + ", Address: " + m_addr.toString(); }

    /* filled by the scan */
    NimBLEAddress m_addr;
    int m_rssi = 0;
    uint8_t m_event = 0;
    std::vector<uint8_t> m_payload;
    bool m_got_rsp = false;

private:
    mutable NimBLEUUID m_sd_uuid;
    /* walk the AD structures: returns the n-th structure of `type` as (ptr, len) */
    bool at(uint8_t type, int n, const uint8_t **d, size_t *len) const {
        size_t i = 0;
        while (i + 1 < m_payload.size()) {
            uint8_t l = m_payload[i];
            if (!l || i + 1 + l > m_payload.size()) break;
            if (m_payload[i + 1] == type && n-- == 0) { *d = &m_payload[i + 2]; *len = l - 1; return true; }
            i += 1 + l;
        }
        return false;
    }
    bool find(uint8_t type) const { const uint8_t *d; size_t l; return at(type, 0, &d, &l); }
    size_t count(uint8_t type) const { const uint8_t *d; size_t l; size_t n = 0; while (at(type, n, &d, &l)) n++; return n; }
    std::string field(uint8_t type, int n) const { const uint8_t *d; size_t l; return at(type, n, &d, &l) ? std::string((const char *)d, l) : std::string(); }
    std::vector<NimBLEUUID> services() const {
        std::vector<NimBLEUUID> v;
        const uint8_t *d;
        size_t l;
        for (uint8_t t : {0x02, 0x03}) for (int n = 0; at(t, n, &d, &l); n++) for (size_t k = 0; k + 1 < l; k += 2) v.push_back(NimBLEUUID((uint16_t)(d[k] | (d[k + 1] << 8))));
        for (uint8_t t : {0x04, 0x05}) for (int n = 0; at(t, n, &d, &l); n++) for (size_t k = 0; k + 3 < l; k += 4) v.push_back(NimBLEUUID((uint32_t)(d[k] | (d[k + 1] << 8) | (d[k + 2] << 16) | ((uint32_t)d[k + 3] << 24))));
        for (uint8_t t : {0x06, 0x07}) for (int n = 0; at(t, n, &d, &l); n++) for (size_t k = 0; k + 15 < l; k += 16) v.push_back(NimBLEUUID(d + k, 128));
        return v;
    }
    std::string serviceData(size_t i) const {
        const uint8_t *d;
        size_t l;
        int n16 = count(0x16), n32 = count(0x20);
        if ((int)i < n16 && at(0x16, i, &d, &l) && l >= 2) { m_sd_uuid = NimBLEUUID((uint16_t)(d[0] | (d[1] << 8))); return std::string((const char *)d + 2, l - 2); }
        i -= n16;
        if ((int)i < n32 && at(0x20, i, &d, &l) && l >= 4) { m_sd_uuid = NimBLEUUID((uint32_t)(d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24))); return std::string((const char *)d + 4, l - 4); }
        i -= n32;
        if (at(0x21, i, &d, &l) && l >= 16) { m_sd_uuid = NimBLEUUID(d, 128); return std::string((const char *)d + 16, l - 16); }
        return std::string();
    }
};

class NimBLEAdvertisedDeviceCallbacks {
public:
    virtual ~NimBLEAdvertisedDeviceCallbacks() {}
    virtual void onResult(NimBLEAdvertisedDevice *d) = 0;
};

class NimBLEScanResults {
public:
    int getCount() { return m_devices.size(); }
    NimBLEAdvertisedDevice getDevice(uint32_t i) { return *m_devices[i]; }
    std::vector<NimBLEAdvertisedDevice *>::iterator begin() { return m_devices.begin(); }
    std::vector<NimBLEAdvertisedDevice *>::iterator end() { return m_devices.end(); }
    std::vector<NimBLEAdvertisedDevice *> m_devices;
};

class NimBLEScan {
public:
    void setAdvertisedDeviceCallbacks(NimBLEAdvertisedDeviceCallbacks *cb, bool wantDuplicates = false) { m_cb = cb; m_dups = wantDuplicates; }
    void setActiveScan(bool a) { m_active = a; }
    void setInterval(uint16_t ms) { m_itvl = ms; }
    void setWindow(uint16_t ms) { m_win = ms; }
    void setMaxResults(uint8_t n) { m_max = n; }
    void setDuplicateFilter(bool f) { m_dups = !f; }
    void setFilterPolicy(uint8_t) {}
    void setLimitedOnly(bool) {}

    bool start(uint32_t seconds, void (*done)(NimBLEScanResults), bool is_continue = false) {
        if (!is_continue) clearResults();
        m_done = done;
        m_running = cpos_api->ble.scan_start(seconds * 1000, m_active, m_itvl, m_win, !m_dups);
        return m_running;
    }
    NimBLEScanResults start(uint32_t seconds, bool is_continue = false) {
        if (start(seconds, nullptr, is_continue))
            while (isScanning()) cpos_api->delay_ms(20);
        return m_results;
    }
    bool stop() {
        cpos_api->ble.scan_stop();
        pump();
        bool was = m_running;
        m_running = false;
        if (was && m_done) m_done(m_results);
        return true;
    }
    bool isScanning() { pump(); return m_running; }
    NimBLEScanResults getResults() { return m_results; }
    void clearResults() {
        for (auto *d : m_results.m_devices) delete d;
        m_results.m_devices.clear();
    }
    void erase(const NimBLEAddress &a) {
        auto &v = m_results.m_devices;
        for (size_t i = 0; i < v.size(); i++) if (v[i]->m_addr == a) { delete v[i]; v.erase(v.begin() + i); return; }
    }

    /* drain the OS queue into devices and callbacks */
    void pump() {
        cpos_ble_adv_t r;
        while (cpos_api->ble.scan_read(&r)) {
            NimBLEAddress a(r.addr, r.addr_type);
            NimBLEAdvertisedDevice *d = nullptr;
            for (auto *x : m_results.m_devices) if (x->m_addr == a) { d = x; break; }
            bool fresh = !d;
            if (fresh) {
                if (m_max == 0) d = &m_tmp;
                else {
                    if (m_results.m_devices.size() >= m_max) { delete m_results.m_devices.front(); m_results.m_devices.erase(m_results.m_devices.begin()); }
                    d = new NimBLEAdvertisedDevice();
                    m_results.m_devices.push_back(d);
                }
                if (m_max == 0 && !(m_tmp.m_addr == a)) m_tmp.m_payload.clear();
                d->m_addr = a;
            }
            d->m_rssi = r.rssi;
            if (r.event == 4) {          /* scan response: append to what the advertisement said */
                d->m_got_rsp = true;
                d->m_payload.insert(d->m_payload.end(), r.data, r.data + r.len);
            } else {
                d->m_event = r.event;
                d->m_got_rsp = false;
                d->m_payload.assign(r.data, r.data + r.len);
            }
            if (m_cb) m_cb->onResult(d);
        }
        if (m_running && !cpos_api->ble.scanning()) {
            m_running = false;
            if (m_done) m_done(m_results);
        }
    }

private:
    NimBLEAdvertisedDeviceCallbacks *m_cb = nullptr;
    void (*m_done)(NimBLEScanResults) = nullptr;
    bool m_dups = false, m_active = false, m_running = false;
    uint16_t m_itvl = 100, m_win = 100;
    uint8_t m_max = 40;
    NimBLEScanResults m_results;
    NimBLEAdvertisedDevice m_tmp;
};

/* ---- client ------------------------------------------------------------------------- */
class NimBLEClient;
class NimBLERemoteCharacteristic;
typedef void (*notify_callback)(NimBLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify);

class NimBLERemoteCharacteristic {
public:
    NimBLERemoteCharacteristic(const cpos_ble_attr_t &a) : m_attr(a), m_uuid(a.uuid, a.uuid_bits) {}
    NimBLEUUID getUUID() const { return m_uuid; }
    uint16_t getHandle() const { return m_attr.handle; }
    bool canRead() const { return m_attr.props & 0x02; }
    bool canWriteNoResponse() const { return m_attr.props & 0x04; }
    bool canWrite() const { return m_attr.props & 0x08; }
    bool canNotify() const { return m_attr.props & 0x10; }
    bool canIndicate() const { return m_attr.props & 0x20; }
    bool canBroadcast() const { return m_attr.props & 0x01; }

    std::string readValue() {
        uint8_t buf[244];
        int n = cpos_api->ble.read(m_attr.handle, buf, sizeof(buf));
        m_value = n > 0 ? std::string((const char *)buf, n) : std::string();
        return m_value;
    }
    uint8_t readUInt8() { std::string v = readValue(); return v.empty() ? 0 : (uint8_t)v[0]; }
    uint16_t readUInt16() { std::string v = readValue(); return v.size() < 2 ? 0 : (uint8_t)v[0] | ((uint8_t)v[1] << 8); }
    uint32_t readUInt32() { std::string v = readValue(); uint32_t r = 0; for (size_t i = 0; i < 4 && i < v.size(); i++) r |= (uint32_t)(uint8_t)v[i] << (8 * i); return r; }
    std::string getValue() const { return m_value; }

    bool writeValue(const uint8_t *data, size_t len, bool response = false) { return cpos_api->ble.write(m_attr.handle, data, len, response) == 0; }
    bool writeValue(const std::string &s, bool response = false) { return writeValue((const uint8_t *)s.data(), s.size(), response); }
    template <typename T> bool writeValue(const T &v, bool response = false) { return writeValue((const uint8_t *)&v, sizeof(T), response); }

    bool subscribe(bool notifications = true, notify_callback cb = nullptr, bool response = false) {
        m_cb = cb;
        return cpos_api->ble.subscribe(m_attr.handle, m_attr.end_handle, notifications ? 1 : 2) == 0;
    }
    bool unsubscribe(bool response = false) { m_cb = nullptr; return cpos_api->ble.subscribe(m_attr.handle, m_attr.end_handle, 0) == 0; }
    bool registerForNotify(notify_callback cb, bool notifications = true, bool response = true) { return cb ? subscribe(notifications, cb, response) : unsubscribe(response); }

    notify_callback m_cb = nullptr;

private:
    cpos_ble_attr_t m_attr;
    NimBLEUUID m_uuid;
    std::string m_value;
};

class NimBLERemoteService {
public:
    NimBLERemoteService(int index, const cpos_ble_attr_t &a) : m_index(index), m_uuid(a.uuid, a.uuid_bits) {}
    ~NimBLERemoteService() { for (auto *c : m_chrs) delete c; }
    NimBLEUUID getUUID() const { return m_uuid; }
    std::vector<NimBLERemoteCharacteristic *> *getCharacteristics(bool refresh = false) {
        if (!m_loaded || refresh) {
            for (auto *c : m_chrs) delete c;
            m_chrs.clear();
            cpos_ble_attr_t a[24];
            int n = cpos_api->ble.characteristics(m_index, a, 24);
            for (int i = 0; i < n; i++) m_chrs.push_back(new NimBLERemoteCharacteristic(a[i]));
            m_loaded = true;
        }
        return &m_chrs;
    }
    NimBLERemoteCharacteristic *getCharacteristic(const NimBLEUUID &u) {
        for (auto *c : *getCharacteristics()) if (c->getUUID() == u) return c;
        return nullptr;
    }
    NimBLERemoteCharacteristic *getCharacteristic(const char *u) { return getCharacteristic(NimBLEUUID(u)); }

private:
    int m_index;
    NimBLEUUID m_uuid;
    bool m_loaded = false;
    std::vector<NimBLERemoteCharacteristic *> m_chrs;
};

class NimBLEClientCallbacks {
public:
    virtual ~NimBLEClientCallbacks() {}
    virtual void onConnect(NimBLEClient *c) {}
    virtual void onDisconnect(NimBLEClient *c) {}
};

class NimBLEClient {
public:
    ~NimBLEClient() { clear(); }
    void setConnectTimeout(uint32_t seconds) { m_timeout = seconds; }
    void setClientCallbacks(NimBLEClientCallbacks *cb, bool deleteCb = true) { m_cbs = cb; }
    bool connect(const NimBLEAddress &a, bool deleteAttributes = true) {
        if (deleteAttributes) clear();
        m_peer = a;
        bool ok = cpos_api->ble.connect(a.getNative(), a.getType(), m_timeout * 1000) == 0;
        m_was = ok;
        if (ok && m_cbs) m_cbs->onConnect(this);
        return ok;
    }
    bool connect(NimBLEAdvertisedDevice *d, bool deleteAttributes = true) { return connect(d->getAddress(), deleteAttributes); }
    bool connect(bool deleteAttributes = true) { return connect(m_peer, deleteAttributes); }
    int disconnect(uint8_t reason = 0) {
        cpos_api->ble.disconnect();
        check();
        return 0;
    }
    bool isConnected() { pump(); return cpos_api->ble.connected(); }
    NimBLEAddress getPeerAddress() const { return m_peer; }

    std::vector<NimBLERemoteService *> *getServices(bool refresh = false) {
        if (refresh || m_svcs.empty()) {
            clear();
            cpos_ble_attr_t a[16];
            int n = cpos_api->ble.services(a, 16);
            for (int i = 0; i < n; i++) m_svcs.push_back(new NimBLERemoteService(i, a[i]));
        }
        return &m_svcs;
    }
    bool discoverAttributes() { getServices(true); for (auto *s : m_svcs) s->getCharacteristics(true); return true; }
    NimBLERemoteService *getService(const NimBLEUUID &u) {
        for (auto *s : *getServices()) if (s->getUUID() == u) return s;
        return nullptr;
    }
    NimBLERemoteService *getService(const char *u) { return getService(NimBLEUUID(u)); }

    /* deliver queued notifications to the subscribed characteristics */
    void pump() {
        uint16_t h;
        uint8_t buf[64];
        int n;
        while ((n = cpos_api->ble.notify_read(&h, buf, sizeof(buf))) >= 0)
            for (auto *s : m_svcs)
                for (auto *c : *s->getCharacteristics())
                    if (c->getHandle() == h && c->m_cb) c->m_cb(c, buf, n, true);
        check();
    }

private:
    void check() {
        if (m_was && !cpos_api->ble.connected()) {
            m_was = false;
            if (m_cbs) m_cbs->onDisconnect(this);
        }
    }
    void clear() { for (auto *s : m_svcs) delete s; m_svcs.clear(); }
    NimBLEAddress m_peer;
    uint32_t m_timeout = 30;
    bool m_was = false;
    NimBLEClientCallbacks *m_cbs = nullptr;
    std::vector<NimBLERemoteService *> m_svcs;
};

/* ---- device ----------------------------------------------------------------------------- */
class NimBLEDevice {
public:
    static bool init(const std::string &name = "") {
        if (!cpos_api->ble.begin()) return false;
        cpos_loop_hook = pumpAll;
        return true;
    }
    static void deinit(bool clearAll = false) {
        cpos_loop_hook = nullptr;
        cpos_api->ble.end();
    }
    static bool getInitialized() { return cpos_loop_hook == pumpAll; }
    static NimBLEScan *getScan() { static NimBLEScan scan; s_scan = &scan; return &scan; }
    static NimBLEClient *createClient() { s_client = new NimBLEClient(); return s_client; }
    static bool deleteClient(NimBLEClient *c) { if (c == s_client) s_client = nullptr; delete c; return true; }
    static void setPower(esp_power_level_t level, int type = 0) { cpos_api->ble.set_power(level); }
    static void setSecurityAuth(bool, bool, bool) {}
    static void setMTU(uint16_t) {}

    static void pumpAll() {
        if (s_scan) s_scan->pump();
        if (s_client) s_client->pump();
    }

private:
    static inline NimBLEScan *s_scan;
    static inline NimBLEClient *s_client;
};
