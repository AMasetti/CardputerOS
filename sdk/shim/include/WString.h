// Arduino-compatible String on top of the app heap.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

class __FlashStringHelper;
#define F(s) (reinterpret_cast<const __FlashStringHelper *>(s))
#define PSTR(s) (s)

class String {
public:
    String() { init(); }
    String(const char *c) { init(); if (c) copy(c, strlen(c)); }
    String(const __FlashStringHelper *c) { init(); if (c) copy((const char *)c, strlen((const char *)c)); }
    String(const String &s) { init(); copy(s.buf, s.len); }
    String(String &&s) noexcept { buf = s.buf; len = s.len; cap = s.cap; s.init(); }
    String(char c) { init(); char t[2] = {c, 0}; copy(t, 1); }
    explicit String(int v, unsigned char base = 10) { init(); numStr((long)v, base); }
    explicit String(unsigned int v, unsigned char base = 10) { init(); numStrU(v, base); }
    explicit String(long v, unsigned char base = 10) { init(); numStr(v, base); }
    explicit String(unsigned long v, unsigned char base = 10) { init(); numStrU(v, base); }
    explicit String(long long v, unsigned char base = 10) { init(); numStr((long)v, base); }
    explicit String(unsigned long long v, unsigned char base = 10) { init(); numStrU((unsigned long)v, base); }
    explicit String(float v, unsigned char dec = 2) { init(); fstr(v, dec); }
    explicit String(double v, unsigned char dec = 2) { init(); fstr(v, dec); }
    ~String() { free(buf); }

    String &operator=(const String &s) { if (this != &s) copy(s.buf, s.len); return *this; }
    String &operator=(String &&s) noexcept { if (this != &s) { free(buf); buf = s.buf; len = s.len; cap = s.cap; s.init(); } return *this; }
    String &operator=(const char *c) { if (c) copy(c, strlen(c)); else { len = 0; if (buf) buf[0] = 0; } return *this; }

    bool reserve(unsigned n) { return grow(n); }
    unsigned length() const { return len; }
    const char *c_str() const { return buf ? buf : ""; }
    bool isEmpty() const { return len == 0; }

    String &operator+=(const String &s) { append(s.buf, s.len); return *this; }
    String &operator+=(const char *c) { if (c) append(c, strlen(c)); return *this; }
    String &operator+=(char c) { append(&c, 1); return *this; }
    String &operator+=(int v) { return *this += String(v); }
    String &operator+=(unsigned v) { return *this += String(v); }
    String &operator+=(long v) { return *this += String(v); }
    String &operator+=(unsigned long v) { return *this += String(v); }
    String &operator+=(float v) { return *this += String(v); }
    String &operator+=(double v) { return *this += String(v); }
    bool concat(const String &s) { append(s.buf, s.len); return true; }
    bool concat(const char *c) { if (c) append(c, strlen(c)); return true; }
    bool concat(char c) { append(&c, 1); return true; }

    friend String operator+(const String &a, const String &b) { String r(a); r += b; return r; }
    friend String operator+(const String &a, const char *b) { String r(a); r += b; return r; }
    friend String operator+(const char *a, const String &b) { String r(a); r += b; return r; }
    friend String operator+(const String &a, char b) { String r(a); r += b; return r; }
    friend String operator+(const String &a, int b) { String r(a); r += String(b); return r; }
    friend String operator+(const String &a, unsigned b) { String r(a); r += String(b); return r; }
    friend String operator+(const String &a, long b) { String r(a); r += String(b); return r; }
    friend String operator+(const String &a, unsigned long b) { String r(a); r += String(b); return r; }
    friend String operator+(const String &a, float b) { String r(a); r += String(b); return r; }
    friend String operator+(const String &a, double b) { String r(a); r += String(b); return r; }

    int compareTo(const String &s) const { return strcmp(c_str(), s.c_str()); }
    bool equals(const String &s) const { return len == s.len && strcmp(c_str(), s.c_str()) == 0; }
    bool equals(const char *c) const { return strcmp(c_str(), c ? c : "") == 0; }
    bool equalsIgnoreCase(const String &s) const { return strcasecmp(c_str(), s.c_str()) == 0; }
    bool operator==(const String &s) const { return equals(s); }
    bool operator==(const char *c) const { return equals(c); }
    bool operator!=(const String &s) const { return !equals(s); }
    bool operator!=(const char *c) const { return !equals(c); }
    bool operator<(const String &s) const { return compareTo(s) < 0; }
    bool operator>(const String &s) const { return compareTo(s) > 0; }
    bool operator<=(const String &s) const { return compareTo(s) <= 0; }
    bool operator>=(const String &s) const { return compareTo(s) >= 0; }

    char charAt(unsigned i) const { return i < len ? buf[i] : 0; }
    char operator[](unsigned i) const { return charAt(i); }
    char &operator[](unsigned i) { static char dummy; if (i >= len) { dummy = 0; return dummy; } return buf[i]; }
    void setCharAt(unsigned i, char c) { if (i < len) buf[i] = c; }

    bool startsWith(const String &p) const { return len >= p.len && strncmp(c_str(), p.c_str(), p.len) == 0; }
    bool endsWith(const String &p) const { return len >= p.len && strcmp(c_str() + len - p.len, p.c_str()) == 0; }
    int indexOf(char c, unsigned from = 0) const {
        if (from >= len) return -1;
        const char *p = strchr(buf + from, c);
        return p ? (int)(p - buf) : -1;
    }
    int indexOf(const String &s, unsigned from = 0) const {
        if (from >= len) return -1;
        const char *p = strstr(buf + from, s.c_str());
        return p ? (int)(p - buf) : -1;
    }
    int lastIndexOf(char c) const {
        if (!buf) return -1;
        const char *p = strrchr(buf, c);
        return p ? (int)(p - buf) : -1;
    }
    String substring(unsigned from, unsigned to) const {
        if (from > to) { unsigned t = from; from = to; to = t; }
        if (from >= len) return String();
        if (to > len) to = len;
        String r;
        r.copy(buf + from, to - from);
        return r;
    }
    String substring(unsigned from) const { return substring(from, len); }
    void toLowerCase() { for (unsigned i = 0; i < len; i++) if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] += 32; }
    void toUpperCase() { for (unsigned i = 0; i < len; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32; }
    void trim() {
        if (!len) return;
        unsigned b = 0, e = len;
        while (b < e && (unsigned char)buf[b] <= ' ') b++;
        while (e > b && (unsigned char)buf[e - 1] <= ' ') e--;
        if (b) memmove(buf, buf + b, e - b);
        len = e - b;
        buf[len] = 0;
    }
    void remove(unsigned idx, unsigned count = 0xFFFFFFFF) {
        if (idx >= len) return;
        if (count > len - idx) count = len - idx;
        memmove(buf + idx, buf + idx + count, len - idx - count + 1);
        len -= count;
    }
    void replace(char a, char b) { for (unsigned i = 0; i < len; i++) if (buf[i] == a) buf[i] = b; }
    void replace(const String &a, const String &b) {
        if (!len || !a.len) return;
        String out;
        unsigned i = 0;
        while (i < len) {
            if (strncmp(buf + i, a.c_str(), a.len) == 0) { out += b; i += a.len; }
            else out += buf[i++];
        }
        *this = out;
    }
    long toInt() const { return buf ? atol(buf) : 0; }
    float toFloat() const { return buf ? (float)atof(buf) : 0; }
    double toDouble() const { return buf ? atof(buf) : 0; }
    void getBytes(unsigned char *out, unsigned n, unsigned from = 0) const {
        if (!n) return;
        if (from >= len) { out[0] = 0; return; }
        unsigned m = len - from;
        if (m > n - 1) m = n - 1;
        memcpy(out, buf + from, m);
        out[m] = 0;
    }
    void toCharArray(char *out, unsigned n, unsigned from = 0) const { getBytes((unsigned char *)out, n, from); }

private:
    char *buf;
    unsigned len, cap;
    void init() { buf = nullptr; len = cap = 0; }
    bool grow(unsigned n) {
        if (n <= cap && buf) return true;
        char *nb = (char *)realloc(buf, n + 1);
        if (!nb) return false;
        if (!buf) nb[0] = 0;
        buf = nb;
        cap = n;
        return true;
    }
    void copy(const char *c, unsigned n) {
        if (!grow(n)) return;
        memmove(buf, c, n);
        buf[n] = 0;
        len = n;
    }
    void append(const char *c, unsigned n) {
        if (!n) return;
        if (!grow(len + n)) return;
        memmove(buf + len, c, n);
        len += n;
        buf[len] = 0;
    }
    void numStr(long v, unsigned char base) {
        char t[40];
        if (base == 10) snprintf(t, sizeof(t), "%ld", v);
        else numU((unsigned long)v, base, t);
        copy(t, strlen(t));
    }
    void numStrU(unsigned long v, unsigned char base) {
        char t[40];
        if (base == 10) snprintf(t, sizeof(t), "%lu", v);
        else numU(v, base, t);
        copy(t, strlen(t));
    }
    static void numU(unsigned long v, unsigned char base, char *out) {
        char tmp[34];
        int n = 0;
        if (!v) tmp[n++] = '0';
        while (v) { int d = v % base; tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; }
        int i = 0;
        while (n) out[i++] = tmp[--n];
        out[i] = 0;
    }
    void fstr(double v, unsigned char dec) {
        char t[48];
        snprintf(t, sizeof(t), "%.*f", dec, v);
        copy(t, strlen(t));
    }
};
