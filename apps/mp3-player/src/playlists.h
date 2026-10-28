// Playlists: un .m3u por lista en /music/Playlists, editables también desde
// la compu. Cada cambio se guarda en el momento.
#pragma once

#include <Arduino.h>
#include <string>
#include <vector>

namespace pls {

static constexpr const char *DIR_PATH = "/music/Playlists";

struct Playlist {
    std::string name;
    std::string ext = ".m3u";
    std::vector<uint16_t> tracks;
};

extern std::vector<Playlist> lists; // ordenadas por nombre

void loadAll();
int find(const std::string &name);
int create(const std::string &name);                 // índice nuevo o -1
int rename(int i, const std::string &name);          // índice nuevo o -1
bool remove(int i);
bool save(int i);
void add(int i, const std::vector<uint16_t> &tracks);
std::string cleanName(const std::string &name);

} // namespace pls
