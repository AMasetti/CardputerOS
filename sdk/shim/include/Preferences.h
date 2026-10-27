#pragma once
#include "Arduino.h"

class Preferences {
public:
    bool begin(const char *name, bool readOnly = false) {
        end();
        h_ = cpos_api->prefs.open(name, readOnly);
        return h_ >= 0;
    }
    void end() { if (h_ >= 0) { cpos_api->prefs.close(h_); h_ = -1; } }
    bool clear() { return cpos_api->prefs.clear(h_) == 0; }
    bool remove(const char *k) { return cpos_api->prefs.remove(h_, k) == 0; }
    bool isKey(const char *k) { size_t n = 0; uint8_t b[1]; return cpos_api->prefs.get(h_, k, 0, b, &n) == 0; }

    size_t putBool(const char *k, bool v) { return put(k, CPOS_NVS_U8, &v, 1); }
    size_t putChar(const char *k, int8_t v) { return put(k, CPOS_NVS_U8, &v, 1); }
    size_t putUChar(const char *k, uint8_t v) { return put(k, CPOS_NVS_U8, &v, 1); }
    size_t putShort(const char *k, int16_t v) { return put(k, CPOS_NVS_U16, &v, 2); }
    size_t putUShort(const char *k, uint16_t v) { return put(k, CPOS_NVS_U16, &v, 2); }
    size_t putInt(const char *k, int32_t v) { return put(k, CPOS_NVS_I32, &v, 4); }
    size_t putUInt(const char *k, uint32_t v) { return put(k, CPOS_NVS_U32, &v, 4); }
    size_t putLong(const char *k, int32_t v) { return putInt(k, v); }
    size_t putULong(const char *k, uint32_t v) { return putUInt(k, v); }
    size_t putFloat(const char *k, float v) { return put(k, CPOS_NVS_BLOB, &v, 4); }
    size_t putString(const char *k, const char *v) { return put(k, CPOS_NVS_STR, v, strlen(v) + 1) ? strlen(v) : 0; }
    size_t putString(const char *k, const String &v) { return putString(k, v.c_str()); }
    size_t putBytes(const char *k, const void *v, size_t n) { return put(k, CPOS_NVS_BLOB, v, n); }

    bool getBool(const char *k, bool d = false) { uint8_t v; return get(k, &v, 1) ? v != 0 : d; }
    int8_t getChar(const char *k, int8_t d = 0) { int8_t v; return get(k, &v, 1) ? v : d; }
    uint8_t getUChar(const char *k, uint8_t d = 0) { uint8_t v; return get(k, &v, 1) ? v : d; }
    int16_t getShort(const char *k, int16_t d = 0) { int16_t v; return get(k, &v, 2) ? v : d; }
    uint16_t getUShort(const char *k, uint16_t d = 0) { uint16_t v; return get(k, &v, 2) ? v : d; }
    int32_t getInt(const char *k, int32_t d = 0) { int32_t v; return get(k, &v, 4) ? v : d; }
    uint32_t getUInt(const char *k, uint32_t d = 0) { uint32_t v; return get(k, &v, 4) ? v : d; }
    int32_t getLong(const char *k, int32_t d = 0) { return getInt(k, d); }
    uint32_t getULong(const char *k, uint32_t d = 0) { return getUInt(k, d); }
    float getFloat(const char *k, float d = 0) { float v; return get(k, &v, 4) ? v : d; }
    String getString(const char *k, const String &d = String()) {
        size_t n = 0;
        char probe[1];
        cpos_api->prefs.get(h_, k, CPOS_NVS_STR, probe, &n);
        if (n == 0) return d;
        char *tmp = (char *)malloc(n + 1);
        if (!tmp) return d;
        size_t m = n;
        String r = d;
        if (cpos_api->prefs.get(h_, k, CPOS_NVS_STR, tmp, &m) == 0) { tmp[m < n ? m : n] = 0; r = String(tmp); }
        free(tmp);
        return r;
    }
    size_t getBytes(const char *k, void *buf, size_t max) {
        size_t n = max;
        return cpos_api->prefs.get(h_, k, CPOS_NVS_BLOB, buf, &n) == 0 ? n : 0;
    }

private:
    int h_ = -1;
    size_t put(const char *k, int type, const void *v, size_t n) { return cpos_api->prefs.set(h_, k, type, v, n) == 0 ? n : 0; }
    bool get(const char *k, void *out, size_t n) {
        size_t len = n;
        return cpos_api->prefs.get(h_, k, 0, out, &len) == 0 && len == n;
    }
};
