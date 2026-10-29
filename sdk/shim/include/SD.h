// Arduino SD on top of the OS file API. Paths are relative to the card root.
#pragma once
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include "Arduino.h"
#include "SPI.h"

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

class File {
public:
    File() {}
    File(FILE *f, const String &path, const String &name) : f_(f), path_(path), name_(name) {}
    File(DIR *d, const String &path, const String &name) : d_(d), path_(path), name_(name) {}
    File(const File &o) = delete;
    File &operator=(const File &o) = delete;
    File(File &&o) noexcept { move(o); }
    File &operator=(File &&o) noexcept { if (this != &o) { close(); move(o); } return *this; }
    ~File() { close(); }

    operator bool() const { return f_ || d_; }
    void close() {
        if (f_) { fclose(f_); f_ = nullptr; }
        if (d_) { closedir(d_); d_ = nullptr; }
    }
    int read() { if (!f_) return -1; return fgetc(f_); }
    int peek() { if (!f_) return -1; int c = fgetc(f_); if (c >= 0) fseek(f_, -1, SEEK_CUR); return c; }
    size_t read(uint8_t *b, size_t n) { return f_ ? fread(b, 1, n, f_) : 0; }
    size_t readBytes(char *b, size_t n) { return read((uint8_t *)b, n); }
    size_t write(uint8_t c) { return f_ ? fwrite(&c, 1, 1, f_) : 0; }
    size_t write(const uint8_t *b, size_t n) { return f_ ? fwrite(b, 1, n, f_) : 0; }
    size_t print(const char *s) { return f_ ? fwrite(s, 1, strlen(s), f_) : 0; }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); return print(b); }
    size_t print(unsigned v) { char b[16]; snprintf(b, sizeof(b), "%u", v); return print(b); }
    size_t print(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); return print(b); }
    size_t print(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); return print(b); }
    size_t print(float v, int d = 2) { char b[32]; snprintf(b, sizeof(b), "%.*f", d, (double)v); return print(b); }
    size_t print(double v, int d = 2) { char b[32]; snprintf(b, sizeof(b), "%.*f", d, v); return print(b); }
    size_t println(const char *s) { size_t n = print(s); return n + print("\n"); }
    size_t println(const String &s) { return println(s.c_str()); }
    size_t println(int v) { size_t n = print(v); return n + print("\n"); }
    size_t println(unsigned v) { size_t n = print(v); return n + print("\n"); }
    size_t println(long v) { size_t n = print(v); return n + print("\n"); }
    size_t println(unsigned long v) { size_t n = print(v); return n + print("\n"); }
    size_t println(double v, int d = 2) { size_t n = print(v, d); return n + print("\n"); }
    size_t println() { return print("\n"); }
    size_t printf(const char *fmt, ...) {
        if (!f_) return 0;
        char b[256];
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        if (n < 0) return 0;
        if (n >= (int)sizeof(b)) n = sizeof(b) - 1;
        return fwrite(b, 1, n, f_);
    }
    void flush() { if (f_) fflush(f_); }
    int available() { if (!f_) return 0; long p = ftell(f_); return (int)(size() - p); }
    size_t size() {
        if (!f_) return 0;
        struct stat st;
        fflush(f_);
        if (stat(path_.c_str(), &st) == 0) return st.st_size;
        return 0;
    }
    size_t position() { return f_ ? ftell(f_) : 0; }
    bool seek(uint32_t pos) { return f_ && fseek(f_, pos, SEEK_SET) == 0; }
    bool seek(uint32_t pos, int mode) { return f_ && fseek(f_, pos, mode) == 0; }
    const char *name() const { return name_.c_str(); }
    const char *path() const { return path_.c_str(); }
    bool isDirectory() const { return d_ != nullptr; }
    String readString() {
        String s;
        int c;
        while ((c = read()) >= 0) s += (char)c;
        return s;
    }
    String readStringUntil(char term) {
        String s;
        int c;
        while ((c = read()) >= 0 && c != term) s += (char)c;
        return s;
    }
    long parseInt() {
        int c = read();
        while (c >= 0 && !(c >= '0' && c <= '9') && c != '-') c = read();
        bool neg = false;
        if (c == '-') { neg = true; c = read(); }
        long v = 0;
        while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = read(); }
        return neg ? -v : v;
    }
    File openNextFile() {
        if (!d_) return File();
        struct dirent *e;
        while ((e = readdir(d_))) {
            if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
            String full = path_ + "/" + e->d_name;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return File(opendir(full.c_str()), full, e->d_name);
            return File(fopen(full.c_str(), "r"), full, e->d_name);
        }
        return File();
    }
    void rewindDirectory() { if (d_) rewinddir(d_); }

private:
    FILE *f_ = nullptr;
    DIR *d_ = nullptr;
    String path_, name_;
    void move(File &o) { f_ = o.f_; d_ = o.d_; path_ = o.path_; name_ = o.name_; o.f_ = nullptr; o.d_ = nullptr; }
};

class SDClass {
public:
    bool begin(int = -1, SPIClass & = *(SPIClass *)nullptr, uint32_t = 0, const char * = nullptr, uint8_t = 0, bool = false) {
        struct stat st;
        return stat("/sdcard", &st) == 0;
    }
    bool begin(int cs) { return begin(cs, *(SPIClass *)nullptr); }
    void end() {}
    File open(const char *path, const char *mode = FILE_READ) {
        String p = full(path);
        struct stat st;
        if (mode[0] == 'r' && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return File(opendir(p.c_str()), p, base(p));
        const char *m = mode;
        if (mode[0] == 'w') m = "w+";           // Arduino FILE_WRITE: also readable
        else if (mode[0] == 'a') m = "a+";
        FILE *f = fopen(p.c_str(), m);
        if (!f) return File();
        return File(f, p, base(p));
    }
    File open(const String &path, const char *mode = FILE_READ) { return open(path.c_str(), mode); }
    bool exists(const char *path) { struct stat st; return stat(full(path).c_str(), &st) == 0; }
    bool exists(const String &p) { return exists(p.c_str()); }
    bool mkdir(const char *path) { return ::mkdir(full(path).c_str(), 0777) == 0; }
    bool mkdir(const String &p) { return mkdir(p.c_str()); }
    bool remove(const char *path) { return ::remove(full(path).c_str()) == 0; }
    bool remove(const String &p) { return remove(p.c_str()); }
    bool rmdir(const char *path) { return ::remove(full(path).c_str()) == 0; }
    bool rename(const char *a, const char *b) { return ::rename(full(a).c_str(), full(b).c_str()) == 0; }
    uint64_t cardSize() { return 0; }
    uint64_t totalBytes() { return 0; }
    uint64_t usedBytes() { return 0; }
    uint8_t cardType() { return 1; }

private:
    static String full(const char *p) {
        if (strncmp(p, "/sdcard", 7) == 0) return String(p);
        if (p[0] == '/') return String("/sdcard") + p;
        return String("/sdcard/") + p;
    }
    static String base(const String &p) {
        int i = p.lastIndexOf('/');
        return i >= 0 ? p.substring(i + 1) : p;
    }
};
extern SDClass SD;
typedef File fs_File;
