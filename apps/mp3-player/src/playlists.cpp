#include "playlists.h"

#include <SD.h>
#include <dirent.h>
#include <algorithm>

#include "library.h"
#include "util.h"

namespace pls {

std::vector<Playlist> lists;

static std::string filePath(const Playlist &p) { return std::string(DIR_PATH) + "/" + p.name + p.ext; }

// Resuelve "." y ".." de una ruta absoluta.
static std::string normalize(const std::string &path)
{
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string s = path.substr(i, j - i);
        if (s == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!s.empty() && s != ".") {
            parts.push_back(s);
        }
        i = j + 1;
    }
    std::string out;
    for (auto &s : parts) out += "/" + s;
    return out;
}

static int resolve(std::string line)
{
    std::replace(line.begin(), line.end(), '\\', '/');
    if (line[0] == '/') {
        int t = lib::findByPath(normalize(line).c_str());
        if (t >= 0) return t;
        return lib::findByPath(normalize(std::string(lib::MUSIC_DIR) + line).c_str());
    }
    int t = lib::findByPath(normalize(std::string(DIR_PATH) + "/" + line).c_str());
    if (t >= 0) return t;
    return lib::findByPath(normalize(std::string(lib::MUSIC_DIR) + "/" + line).c_str());
}

static void loadOne(Playlist &p)
{
    File f = SD.open(filePath(p).c_str());
    if (!f) return;
    std::string line;
    uint8_t chunk[256];
    auto flush = [&]() {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() >= 3 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) line.erase(0, 3);
        if (!line.empty() && line[0] != '#') {
            int t = resolve(line);
            if (t >= 0) p.tracks.push_back(t);
        }
        line.clear();
    };
    while (f.available()) {
        int n = f.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            if (chunk[i] == '\n') flush();
            else line += (char)chunk[i];
        }
    }
    flush();
    f.close();
}

static void sortLists()
{
    std::sort(lists.begin(), lists.end(), [](const Playlist &a, const Playlist &b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

void loadAll()
{
    lists.clear();
    SD.mkdir(DIR_PATH);
    DIR *d = opendir((std::string("/sd") + DIR_PATH).c_str());
    if (!d) return;
    while (dirent *e = readdir(d)) {
        const char *n = e->d_name;
        const char *ext = strrchr(n, '.');
        if (n[0] == '.' || e->d_type == DT_DIR || !ext) continue;
        if (strcasecmp(ext, ".m3u") && strcasecmp(ext, ".m3u8")) continue;
        Playlist p;
        p.name.assign(n, ext - n);
        p.ext = ext;
        lists.push_back(p);
    }
    closedir(d);
    for (auto &p : lists) loadOne(p);
    sortLists();
}

int find(const std::string &name)
{
    for (size_t i = 0; i < lists.size(); ++i)
        if (!strcasecmp(lists[i].name.c_str(), name.c_str())) return i;
    return -1;
}

std::string cleanName(const std::string &name)
{
    std::string r;
    for (char c : name)
        if (!strchr("/\\:*?\"<>|", c)) r += c;
    size_t a = r.find_first_not_of(' '), b = r.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : r.substr(a, b - a + 1);
}

bool save(int i)
{
    if (i < 0 || i >= (int)lists.size()) return false;
    SD.mkdir(DIR_PATH);
    File f = SD.open(filePath(lists[i]).c_str(), FILE_WRITE);
    if (!f) return false;
    std::string buf = "#EXTM3U\n";
    char p[400];
    for (uint16_t t : lists[i].tracks) {
        lib::trackPath(t, p, sizeof(p));
        buf += "#EXTINF:" + std::to_string(lib::tracks[t].seconds) + "," + lib::artistOf(t) + " - " + lib::title(t) + "\n";
        buf += p;
        buf += "\n";
        if (buf.size() > 2048) {
            f.write((const uint8_t *)buf.data(), buf.size());
            buf.clear();
        }
    }
    f.write((const uint8_t *)buf.data(), buf.size());
    f.close();
    return true;
}

static int indexOf(const std::string &name)
{
    for (size_t i = 0; i < lists.size(); ++i)
        if (lists[i].name == name) return i;
    return -1;
}

int create(const std::string &raw)
{
    std::string name = cleanName(raw);
    if (name.empty() || find(name) >= 0) return -1;
    Playlist p;
    p.name = name;
    lists.push_back(p);
    sortLists();
    int i = indexOf(name);
    if (!save(i)) {
        lists.erase(lists.begin() + i);
        return -1;
    }
    return i;
}

int rename(int i, const std::string &raw)
{
    std::string name = cleanName(raw);
    if (i < 0 || i >= (int)lists.size() || name.empty()) return -1;
    int other = find(name);
    if (other >= 0 && other != i) return -1;
    std::string from = filePath(lists[i]);
    lists[i].name = name;
    std::string to = filePath(lists[i]);
    if (from != to && !SD.rename(from.c_str(), to.c_str())) save(i);
    sortLists();
    return indexOf(name);
}

bool remove(int i)
{
    if (i < 0 || i >= (int)lists.size()) return false;
    SD.remove(filePath(lists[i]).c_str());
    lists.erase(lists.begin() + i);
    return true;
}

void add(int i, const std::vector<uint16_t> &tracks)
{
    if (i < 0 || i >= (int)lists.size()) return;
    lists[i].tracks.insert(lists[i].tracks.end(), tracks.begin(), tracks.end());
    save(i);
}

} // namespace pls
