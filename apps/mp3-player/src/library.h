// Biblioteca de música: recorre /music, lee los tags ID3 y agrupa por
// artista y disco. El índice se guarda en /music/.mp3player/library.tsv y en
// cada arranque solo se leen los tags de los archivos nuevos.
#pragma once

#include <Arduino.h>
#include <string>
#include <vector>

namespace lib {

static constexpr const char *MUSIC_DIR = "/music";
static constexpr const char *DATA_DIR = "/music/.mp3player";
static constexpr size_t MAX_TRACKS = 4000;

struct Track {
    uint32_t file;     // nombre del archivo dentro de su carpeta
    uint32_t title;
    uint32_t artist;   // artista del tema (TPE1)
    uint32_t audioOff; // byte donde empieza el audio, después del ID3v2
    uint16_t dir;
    uint16_t album;
    uint16_t seconds;
    uint8_t disc;
    uint8_t num;
};

struct Album {
    uint32_t name;
    uint16_t artist;
    uint16_t year;
    std::vector<uint16_t> tracks; // por disco y número
};

struct Artist {
    uint32_t name;
    std::vector<uint16_t> albums; // por año y nombre
};

extern std::vector<Track> tracks;
extern std::vector<Album> albums;
extern std::vector<Artist> artists;
extern std::vector<uint16_t> artistOrder; // alfabético
extern std::vector<uint16_t> albumOrder;  // alfabético
extern std::vector<uint16_t> songOrder;   // alfabético por título

using Progress = void (*)(const char *msg, int done, int total);

// Carga el índice y lo actualiza con los cambios de la SD.
// rebuild = true vuelve a leer los tags de todos los archivos.
bool load(bool rebuild, Progress cb);

const char *str(uint32_t off);
inline const char *title(int t) { return str(tracks[t].title); }
inline const char *artistOf(int t) { return str(tracks[t].artist); }
inline const char *albumName(int al) { return str(albums[al].name); }
inline const char *artistName(int ar) { return str(artists[ar].name); }

// Ruta completa en la SD ("/music/...").
void trackPath(int t, char *buf, size_t n);
std::string trackDir(int t);
int findByPath(const char *path);

// Temas de un artista, disco por disco.
void artistTracks(int artist, std::vector<uint16_t> &out);

// Ubicación de la imagen embebida (APIC) en el archivo de un tema.
bool findPicture(int t, uint32_t &off, uint32_t &len);

} // namespace lib
