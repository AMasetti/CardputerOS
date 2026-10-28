#include "art.h"

#include <SD.h>
#include <dirent.h>
#include <string>

#include "library.h"
#include "util.h"

namespace art {

static const char *ART_DIR = "/music/.mp3player/art";

// Un tramo de un archivo (la imagen dentro del MP3) visto como archivo entero.
struct FileSlice : public lgfx::v1::DataWrapper {
    File f;
    uint32_t base = 0, len = 0, pos = 0;

    int read(uint8_t *buf, uint32_t n) override
    {
        if (n > len - pos) n = len - pos;
        int r = f.read(buf, n);
        if (r > 0) pos += r;
        return r;
    }
    void skip(int32_t off) override { seek(pos + off); }
    bool seek(uint32_t off) override
    {
        pos = off < len ? off : len;
        return f.seek(base + pos);
    }
    void close() override
    {
        if (f) f.close();
    }
    int32_t tell() override { return pos; }
};

static bool drawImage(LGFX_Sprite &spr, const char *path, uint32_t off, uint32_t len)
{
    FileSlice s;
    s.f = SD.open(path);
    if (!s.f) return false;
    uint32_t size = s.f.size();
    if (off >= size) return false;
    s.base = off;
    s.len = (len && off + len <= size) ? len : size - off;
    uint8_t magic[2] = {0, 0};
    s.seek(0);
    s.f.read(magic, 2);
    s.seek(0);

    spr.fillSprite(TFT_BLACK);
    bool ok = false;
    if (magic[0] == 0xFF && magic[1] == 0xD8)
        ok = spr.drawJpg(&s, 0, 0, SIZE, SIZE, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    else if (magic[0] == 0x89 && magic[1] == 'P')
        ok = spr.drawPng(&s, 0, 0, SIZE, SIZE, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    s.close();
    return ok;
}

// 3 = cover, 2 = folder/front, 1 = cualquier otra imagen.
static int imageRank(const char *n)
{
    if (n[0] == '.') return 0;
    const char *ext = strrchr(n, '.');
    if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".png"))) return 0;
    if (!strncasecmp(n, "cover", 5)) return 3;
    if (!strncasecmp(n, "folder", 6) || !strncasecmp(n, "front", 5)) return 2;
    return 1;
}

static bool generate(int album, LGFX_Sprite &spr)
{
    const lib::Album &al = lib::albums[album];
    if (al.tracks.empty()) return false;

    std::string dir = lib::trackDir(al.tracks[0]);
    std::string best;
    int bestRank = 0;
    if (DIR *d = opendir(("/sd" + dir).c_str())) {
        while (dirent *e = readdir(d)) {
            if (e->d_type == DT_DIR) continue;
            int r = imageRank(e->d_name);
            if (r > bestRank) bestRank = r, best = e->d_name;
        }
        closedir(d);
    }
    if (bestRank >= 2 && drawImage(spr, (dir + "/" + best).c_str(), 0, 0)) return true;

    for (size_t i = 0; i < al.tracks.size() && i < 3; ++i) {
        uint32_t off, len;
        if (!lib::findPicture(al.tracks[i], off, len)) continue;
        char p[400];
        lib::trackPath(al.tracks[i], p, sizeof(p));
        if (drawImage(spr, p, off, len)) return true;
    }
    return bestRank == 1 && drawImage(spr, (dir + "/" + best).c_str(), 0, 0);
}

bool load(int album, LGFX_Sprite &spr)
{
    if (album < 0 || album >= (int)lib::albums.size()) return false;
    const lib::Album &al = lib::albums[album];
    uint32_t key = util::fnv1a(lib::albumName(album), util::fnv1a(lib::artistName(al.artist)));
    char cpath[64];
    snprintf(cpath, sizeof(cpath), "%s/%08lx.565", ART_DIR, (unsigned long)key);

    const size_t bytes = SIZE * SIZE * 2;
    uint8_t *px = (uint8_t *)spr.getBuffer();
    if (File c = SD.open(cpath)) {
        size_t sz = c.size();
        bool ok = sz == bytes && c.read(px, bytes) == (int)bytes;
        c.close();
        if (sz == 1) return false; // ya se buscó y no tiene tapa
        if (ok) return true;
    }

    bool ok = generate(album, spr);
    SD.mkdir(lib::DATA_DIR);
    SD.mkdir(ART_DIR);
    if (File w = SD.open(cpath, FILE_WRITE)) {
        if (ok) w.write(px, bytes);
        else w.write((uint8_t)0);
        w.close();
    }
    return ok;
}

void clearCache()
{
    DIR *d = opendir((std::string("/sd") + ART_DIR).c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent *e = readdir(d))
        if (e->d_type != DT_DIR) names.push_back(e->d_name);
    closedir(d);
    for (auto &n : names) SD.remove((std::string(ART_DIR) + "/" + n).c_str());
}

} // namespace art
