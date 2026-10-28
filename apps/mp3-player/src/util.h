// Utilidades de texto compartidas.
#pragma once

#include <Arduino.h>
#include <string>
#include <strings.h>

namespace util {

inline std::string lower(const std::string &s)
{
    std::string r = s;
    for (char &c : r)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return r;
}

inline uint32_t fnv1a(const char *s, uint32_t h = 2166136261u)
{
    for (; *s; ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        h = (h ^ (uint8_t)c) * 16777619u;
    }
    return h;
}

// Clave de orden estilo iPod: ignora "The " al principio.
inline const char *sortKey(const char *s)
{
    return strncasecmp(s, "the ", 4) == 0 ? s + 4 : s;
}

inline int compareNames(const char *a, const char *b)
{
    return strcasecmp(sortKey(a), sortKey(b));
}

// "m:ss" o "h:mm:ss".
inline void fmtTime(char *buf, size_t n, uint32_t sec)
{
    if (sec >= 3600) snprintf(buf, n, "%u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
    else snprintf(buf, n, "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

// Agrega un code point como UTF-8.
inline void appendUtf8(std::string &out, uint32_t c)
{
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += (char)(0xE0 | c >> 12); out += (char)(0x80 | (c >> 6 & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    else { out += (char)(0xF0 | c >> 18); out += (char)(0x80 | (c >> 12 & 0x3F)); out += (char)(0x80 | (c >> 6 & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
}

} // namespace util
