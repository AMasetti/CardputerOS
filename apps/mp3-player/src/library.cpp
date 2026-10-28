#include "library.h"

#include <SD.h>
#include <dirent.h>
#include <algorithm>
#include <map>
#include <unordered_map>

#include "util.h"

namespace lib {

std::vector<Track> tracks;
std::vector<Album> albums;
std::vector<Artist> artists;
std::vector<uint16_t> artistOrder, albumOrder, songOrder;

static std::vector<char> pool;      // todas las cadenas, terminadas en '\0'
static std::vector<uint32_t> dirs;  // carpeta de cada tema, relativa a /music
static std::vector<std::pair<uint32_t, uint16_t>> pathIndex;

static const char *CACHE_PATH = "/music/.mp3player/library.tsv";
static const char *CACHE_MAGIC = "#MP3LIB 1";

const char *str(uint32_t off) { return pool.data() + off; }

static uint32_t put(const std::string &s)
{
    uint32_t off = pool.size();
    pool.insert(pool.end(), s.begin(), s.end());
    pool.push_back('\0');
    return off;
}

// --- Registro intermedio (de la caché o de los tags) ----------------------
struct Rec {
    std::string rel; // relativo a /music
    std::string title, artist, albumArtist, album;
    uint32_t audioOff = 0;
    uint32_t tlenMs = 0;
    uint16_t seconds = 0, year = 0;
    uint8_t disc = 0, num = 0;
};

// --- ID3v2 ------------------------------------------------------------------
static uint32_t syncsafe(const uint8_t *b) { return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }
static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

static std::string decodeText(const uint8_t *d, size_t n)
{
    std::string out;
    if (!n) return out;
    uint8_t enc = d[0];
    d++, n--;
    if (enc == 0) { // ISO-8859-1
        for (size_t i = 0; i < n && d[i]; ++i) util::appendUtf8(out, d[i]);
    } else if (enc == 3) { // UTF-8
        for (size_t i = 0; i < n && d[i]; ++i) out += (char)d[i];
    } else { // UTF-16 con BOM (1) o big endian (2)
        bool be = enc == 2;
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if (d[0] == 0xFF && d[1] == 0xFE) be = false, i = 2;
            else if (d[0] == 0xFE && d[1] == 0xFF) be = true, i = 2;
        }
        for (; i + 1 < n; i += 2) {
            uint32_t c = be ? d[i] << 8 | d[i + 1] : d[i + 1] << 8 | d[i];
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                uint32_t lo = be ? d[i + 2] << 8 | d[i + 3] : d[i + 3] << 8 | d[i + 2];
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            util::appendUtf8(out, c);
        }
    }
    for (char &c : out)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}

struct PicInfo {
    uint32_t off = 0, len = 0;
    bool front = false;
};

// Lee el tag ID3v2 (2.2, 2.3 y 2.4). Devuelve el byte donde empieza el audio.
static uint32_t parseId3(File &f, Rec *rec, PicInfo *pic)
{
    uint8_t h[10];
    f.seek(0);
    if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
    const uint8_t ver = h[3], flags = h[5];
    const uint32_t size = syncsafe(h + 6);
    const uint32_t audioOff = 10 + size + ((ver == 4 && (flags & 0x10)) ? 10 : 0);
    if (ver < 2 || ver > 4 || (flags & 0x80)) return audioOff; // unsync global: no se lee

    uint32_t pos = 10;
    const uint32_t end = 10 + size;
    if (flags & 0x40) {
        uint8_t e[4];
        f.seek(pos);
        f.read(e, 4);
        pos += ver == 4 ? syncsafe(e) : be32(e) + 4;
    }

    const int hdrLen = ver == 2 ? 6 : 10;
    uint8_t buf[320];
    while (pos + hdrLen <= end) {
        uint8_t fh[10];
        f.seek(pos);
        if (f.read(fh, hdrLen) != hdrLen || fh[0] == 0) break; // padding
        char id[5] = {0};
        uint32_t len;
        uint16_t fflags = 0;
        if (ver == 2) {
            memcpy(id, fh, 3);
            len = fh[3] << 16 | fh[4] << 8 | fh[5];
        } else {
            memcpy(id, fh, 4);
            // Algunos programas escriben tamaños sin syncsafe en 2.4.
            bool plain = ver == 3 || ((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80);
            len = plain ? be32(fh + 4) : syncsafe(fh + 4);
            fflags = fh[8] << 8 | fh[9];
        }
        uint32_t data = pos + hdrLen;
        if (len == 0 || data + len > end) break;
        pos = data + len;

        if (ver == 3 && (fflags & 0x00C0)) continue;  // comprimido / cifrado
        if (ver == 4) {
            if (fflags & 0x000E) continue;           // comprimido / cifrado / unsync
            if (fflags & 0x0001) data += 4, len -= 4; // data length indicator
        }

        bool isPic = !strcmp(id, "APIC") || !strcmp(id, "PIC");
        if (isPic) {
            if (!pic || pic->front) continue;
            uint32_t n = std::min<uint32_t>(len, 200);
            f.seek(data);
            if (f.read(buf, n) != (int)n) continue;
            uint8_t enc = buf[0];
            uint32_t i = 1;
            if (ver == 2) {
                i += 3; // formato "JPG"/"PNG"
            } else {
                while (i < n && buf[i]) ++i; // mime
                ++i;
            }
            uint8_t type = i < n ? buf[i] : 0;
            ++i;
            if (enc == 1 || enc == 2) { // descripción UTF-16: termina en 00 00 alineado
                while (i + 1 < n && (buf[i] || buf[i + 1])) i += 2;
                i += 2;
            } else {
                while (i < n && buf[i]) ++i;
                ++i;
            }
            if (i >= n || i >= len) continue;
            if (!pic->len || type == 3) {
                pic->off = data + i;
                pic->len = len - i;
                pic->front = type == 3;
            }
            continue;
        }
        if (!rec || id[0] != 'T') continue;

        uint32_t n = std::min<uint32_t>(len, sizeof(buf));
        f.seek(data);
        if (f.read(buf, n) != (int)n) continue;
        std::string v = decodeText(buf, n);
        if (v.empty()) continue;

        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) rec->title = v;
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) rec->artist = v;
        else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) rec->albumArtist = v;
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) rec->album = v;
        else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) rec->num = std::min(255, atoi(v.c_str()));
        else if (!strcmp(id, "TPOS") || !strcmp(id, "TPA")) rec->disc = std::min(255, atoi(v.c_str()));
        else if (!strcmp(id, "TLEN") || !strcmp(id, "TLE")) rec->tlenMs = strtoul(v.c_str(), nullptr, 10);
        else if (!strcmp(id, "TYER") || !strcmp(id, "TYE") || !strcmp(id, "TDRC")) rec->year = atoi(v.c_str());
    }
    return audioOff;
}

// Duración: cabecera Xing/Info/VBRI si existe, si no se estima como CBR.
static uint16_t mp3Seconds(File &f, uint32_t audioOff, uint32_t fileSize)
{
    static const uint16_t BR1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t BR2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t SR[3] = {44100, 48000, 32000};
    static uint8_t b[1024];

    f.seek(audioOff);
    int n = f.read(b, sizeof(b));
    for (int i = 0; i + 4 <= n; ++i) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
        int verBits = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3;
        int brIdx = b[i + 2] >> 4, srIdx = (b[i + 2] >> 2) & 3;
        if (verBits == 1 || layer != 1 || brIdx == 0 || brIdx == 15 || srIdx == 3) continue;
        bool mpeg1 = verBits == 3, mono = (b[i + 3] >> 6) == 3;
        uint32_t sr = SR[srIdx] >> (mpeg1 ? 0 : verBits == 2 ? 1 : 2);
        uint32_t spf = mpeg1 ? 1152 : 576;
        int x = i + 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
        uint32_t frames = 0;
        if (x + 12 <= n && (!memcmp(b + x, "Xing", 4) || !memcmp(b + x, "Info", 4)) && (be32(b + x + 4) & 1))
            frames = be32(b + x + 8);
        else if (i + 54 <= n && !memcmp(b + i + 36, "VBRI", 4))
            frames = be32(b + i + 50);
        if (frames) return std::min<uint32_t>(65535, (uint64_t)frames * spf / sr);
        uint32_t kbps = (mpeg1 ? BR1 : BR2)[brIdx];
        uint32_t bytes = fileSize > audioOff + i ? fileSize - audioOff - i : 0;
        return std::min<uint32_t>(65535, (uint64_t)bytes * 8 / (kbps * 1000));
    }
    return 0;
}

// Completa lo que falte con la carpeta y el nombre del archivo
// ("Artista - Disco (Año)/01 - Tema.mp3").
static void fallbacks(Rec &r)
{
    size_t slash = r.rel.rfind('/');
    std::string dir = slash == std::string::npos ? "" : r.rel.substr(0, slash);
    std::string file = slash == std::string::npos ? r.rel : r.rel.substr(slash + 1);
    size_t ds = dir.rfind('/');
    if (ds != std::string::npos) dir = dir.substr(ds + 1);
    size_t dot = file.rfind('.');
    if (dot != std::string::npos) file = file.substr(0, dot);

    if (r.title.empty()) {
        size_t i = 0;
        int num = 0;
        while (i < file.size() && isdigit((uint8_t)file[i])) num = num * 10 + (file[i++] - '0');
        size_t j = i;
        while (j < file.size() && strchr(" -._", file[j])) ++j;
        r.title = (i > 0 && j < file.size()) ? file.substr(j) : file;
        if (!r.num && i > 0) r.num = std::min(255, num);
    }

    std::string dirArtist, dirAlbum = dir;
    size_t dash = dir.find(" - ");
    if (dash != std::string::npos) dirArtist = dir.substr(0, dash), dirAlbum = dir.substr(dash + 3);
    size_t paren = dirAlbum.rfind(" (");
    if (paren != std::string::npos && dirAlbum.back() == ')') {
        if (!r.year) r.year = atoi(dirAlbum.c_str() + paren + 2);
        dirAlbum = dirAlbum.substr(0, paren);
    }
    if (r.artist.empty()) r.artist = dirArtist.empty() ? (r.albumArtist.empty() ? "Unknown Artist" : r.albumArtist) : dirArtist;
    if (r.albumArtist.empty()) r.albumArtist = r.artist;
    if (r.album.empty()) r.album = dirAlbum.empty() ? "Unknown Album" : dirAlbum;
    if (r.year > 9999) r.year = 0;
}

static void readTags(Rec &r)
{
    std::string path = std::string(MUSIC_DIR) + "/" + r.rel;
    File f = SD.open(path.c_str());
    if (f) {
        r.audioOff = parseId3(f, &r, nullptr);
        r.seconds = r.tlenMs ? std::min<uint32_t>(65535, r.tlenMs / 1000) : mp3Seconds(f, r.audioOff, f.size());
        f.close();
    }
    fallbacks(r);
}

// --- Armado de la biblioteca ------------------------------------------------
struct Builder {
    std::map<std::string, uint16_t> dirIdx;
    std::unordered_map<std::string, uint16_t> artistIdx, albumIdx;

    void add(const Rec &r)
    {
        if (tracks.size() >= MAX_TRACKS) return;
        size_t slash = r.rel.rfind('/');
        std::string d = slash == std::string::npos ? "" : r.rel.substr(0, slash);
        std::string f = slash == std::string::npos ? r.rel : r.rel.substr(slash + 1);

        uint16_t di;
        auto dit = dirIdx.find(d);
        if (dit == dirIdx.end()) {
            di = dirs.size();
            dirs.push_back(put(d));
            dirIdx[d] = di;
        } else {
            di = dit->second;
        }

        std::string ak = util::lower(r.albumArtist);
        uint16_t ar;
        auto ait = artistIdx.find(ak);
        if (ait == artistIdx.end()) {
            ar = artists.size();
            artists.push_back({put(r.albumArtist), {}});
            artistIdx[ak] = ar;
        } else {
            ar = ait->second;
        }

        std::string alk = ak + '\x1f' + util::lower(r.album);
        uint16_t al;
        auto alit = albumIdx.find(alk);
        if (alit == albumIdx.end()) {
            al = albums.size();
            albums.push_back({put(r.album), ar, r.year, {}});
            artists[ar].albums.push_back(al);
            albumIdx[alk] = al;
        } else {
            al = alit->second;
            if (!albums[al].year) albums[al].year = r.year;
        }

        Track t;
        t.file = put(f);
        t.title = put(r.title);
        t.artist = r.artist == r.albumArtist ? artists[ar].name : put(r.artist);
        t.audioOff = r.audioOff;
        t.dir = di;
        t.album = al;
        t.seconds = r.seconds;
        t.disc = r.disc;
        t.num = r.num;
        albums[al].tracks.push_back(tracks.size());
        tracks.push_back(t);
    }
};

static void finish()
{
    for (auto &al : albums) {
        std::sort(al.tracks.begin(), al.tracks.end(), [](uint16_t a, uint16_t b) {
            const Track &x = tracks[a], &y = tracks[b];
            if (x.disc != y.disc) return x.disc < y.disc;
            if (x.num != y.num) return x.num < y.num;
            return strcasecmp(str(x.file), str(y.file)) < 0;
        });
    }
    for (auto &ar : artists) {
        std::sort(ar.albums.begin(), ar.albums.end(), [](uint16_t a, uint16_t b) {
            if (albums[a].year != albums[b].year) return albums[a].year < albums[b].year;
            return util::compareNames(albumName(a), albumName(b)) < 0;
        });
    }
    artistOrder.resize(artists.size());
    for (size_t i = 0; i < artists.size(); ++i) artistOrder[i] = i;
    std::sort(artistOrder.begin(), artistOrder.end(), [](uint16_t a, uint16_t b) {
        return util::compareNames(artistName(a), artistName(b)) < 0;
    });
    albumOrder.resize(albums.size());
    for (size_t i = 0; i < albums.size(); ++i) albumOrder[i] = i;
    std::sort(albumOrder.begin(), albumOrder.end(), [](uint16_t a, uint16_t b) {
        return util::compareNames(albumName(a), albumName(b)) < 0;
    });
    songOrder.resize(tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i) songOrder[i] = i;
    std::sort(songOrder.begin(), songOrder.end(), [](uint16_t a, uint16_t b) {
        int c = util::compareNames(title(a), title(b));
        return c ? c < 0 : util::compareNames(artistOf(a), artistOf(b)) < 0;
    });

    pathIndex.clear();
    pathIndex.reserve(tracks.size());
    char p[400];
    for (size_t i = 0; i < tracks.size(); ++i) {
        trackPath(i, p, sizeof(p));
        pathIndex.push_back({util::fnv1a(p), (uint16_t)i});
    }
    std::sort(pathIndex.begin(), pathIndex.end());
    pool.shrink_to_fit();
    tracks.shrink_to_fit();
}

// --- Recorrido de la SD -------------------------------------------------------
static bool isMp3(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".mp3");
}

static void walk(const std::string &rel, int depth, std::vector<std::string> &out)
{
    std::string full = std::string("/sd") + MUSIC_DIR + (rel.empty() ? "" : "/" + rel);
    DIR *d = opendir(full.c_str());
    if (!d) return;
    while (dirent *e = readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.') continue; // ocultos y "._" de macOS
        std::string child = rel.empty() ? std::string(n) : rel + "/" + n;
        if (e->d_type == DT_DIR) {
            if (depth < 4) walk(child, depth + 1, out);
        } else if (isMp3(n)) {
            out.push_back(child);
        }
        if (out.size() >= MAX_TRACKS) break;
    }
    closedir(d);
}

// --- Caché ------------------------------------------------------------------------
// rel \t seg \t audioOff \t disco \t num \t año \t título \t artista \t artista disco \t disco
static bool parseLine(char *line, Rec &r)
{
    char *f[10];
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < 10; ++p)
        if (*p == '\t') *p = '\0', f[n++] = p + 1;
    if (n != 10) return false;
    r.rel = f[0];
    r.seconds = atoi(f[1]);
    r.audioOff = strtoul(f[2], nullptr, 10);
    r.disc = atoi(f[3]);
    r.num = atoi(f[4]);
    r.year = atoi(f[5]);
    r.title = f[6];
    r.artist = f[7];
    r.albumArtist = f[8];
    r.album = f[9];
    return !r.rel.empty();
}

static void writeCache()
{
    SD.mkdir(DATA_DIR);
    File c = SD.open(CACHE_PATH, FILE_WRITE);
    if (!c) return;
    std::string buf = std::string(CACHE_MAGIC) + "\n";
    char nums[64];
    for (size_t i = 0; i < tracks.size(); ++i) {
        const Track &t = tracks[i];
        const Album &al = albums[t.album];
        const char *d = str(dirs[t.dir]);
        if (*d) buf += d, buf += '/';
        buf += str(t.file);
        snprintf(nums, sizeof(nums), "\t%u\t%u\t%u\t%u\t%u\t", t.seconds, (unsigned)t.audioOff, t.disc, t.num, al.year);
        buf += nums;
        buf += str(t.title), buf += '\t';
        buf += str(t.artist), buf += '\t';
        buf += artistName(al.artist), buf += '\t';
        buf += str(al.name), buf += '\n';
        if (buf.size() > 2048) {
            c.write((const uint8_t *)buf.data(), buf.size());
            buf.clear();
        }
    }
    c.write((const uint8_t *)buf.data(), buf.size());
    c.close();
}

bool load(bool rebuild, Progress cb)
{
    tracks.clear();
    albums.clear();
    artists.clear();
    dirs.clear();
    pool.clear();
    pool.reserve(48 * 1024);

    if (cb) cb("Looking for music...", 0, 0);
    std::vector<std::string> files;
    walk("", 0, files);
    std::sort(files.begin(), files.end());
    std::vector<bool> known(files.size(), false);

    Builder b;
    bool changed = rebuild;
    if (!rebuild) {
        File c = SD.open(CACHE_PATH);
        if (!c) changed = true;
        static char line[1024];
        size_t len = 0;
        bool header = true, valid = (bool)c;
        uint8_t chunk[512];
        while (valid && c.available()) {
            int n = c.read(chunk, sizeof(chunk));
            if (n <= 0) break;
            for (int i = 0; i < n; ++i) {
                if (chunk[i] != '\n') {
                    if (len < sizeof(line) - 1) line[len++] = chunk[i];
                    continue;
                }
                line[len] = '\0';
                len = 0;
                if (header) {
                    header = false;
                    if (strcmp(line, CACHE_MAGIC)) { valid = false, changed = true; break; }
                    continue;
                }
                Rec r;
                if (!parseLine(line, r)) { changed = true; continue; }
                auto it = std::lower_bound(files.begin(), files.end(), r.rel);
                size_t k = it - files.begin();
                if (it != files.end() && *it == r.rel && !known[k]) {
                    known[k] = true;
                    b.add(r);
                } else {
                    changed = true; // el archivo ya no está
                }
            }
        }
        if (c) c.close();
    }

    int todo = 0, done = 0;
    for (size_t i = 0; i < files.size(); ++i) todo += !known[i];
    uint32_t lastCb = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        if (known[i]) continue;
        Rec r;
        r.rel = files[i];
        readTags(r);
        b.add(r);
        changed = true;
        ++done;
        if (cb && (millis() - lastCb > 100 || done == todo)) {
            cb("Reading tags...", done, todo);
            lastCb = millis();
        }
    }
    files.clear();
    files.shrink_to_fit();

    finish();
    if (changed) {
        if (cb) cb("Saving library...", 0, 0);
        writeCache();
    }
    return !tracks.empty();
}

// --- Consultas ------------------------------------------------------------------------
void trackPath(int t, char *buf, size_t n)
{
    const char *d = str(dirs[tracks[t].dir]);
    snprintf(buf, n, "%s/%s%s%s", MUSIC_DIR, d, *d ? "/" : "", str(tracks[t].file));
}

std::string trackDir(int t)
{
    const char *d = str(dirs[tracks[t].dir]);
    return std::string(MUSIC_DIR) + (*d ? "/" : "") + d;
}

int findByPath(const char *path)
{
    uint32_t h = util::fnv1a(path);
    auto it = std::lower_bound(pathIndex.begin(), pathIndex.end(), std::make_pair(h, (uint16_t)0));
    char p[400];
    for (; it != pathIndex.end() && it->first == h; ++it) {
        trackPath(it->second, p, sizeof(p));
        if (!strcasecmp(p, path)) return it->second;
    }
    return -1;
}

void artistTracks(int artist, std::vector<uint16_t> &out)
{
    out.clear();
    for (uint16_t al : artists[artist].albums)
        out.insert(out.end(), albums[al].tracks.begin(), albums[al].tracks.end());
}

bool findPicture(int t, uint32_t &off, uint32_t &len)
{
    char p[400];
    trackPath(t, p, sizeof(p));
    File f = SD.open(p);
    if (!f) return false;
    PicInfo pic;
    parseId3(f, nullptr, &pic);
    f.close();
    off = pic.off;
    len = pic.len;
    return pic.len > 0;
}

} // namespace lib
