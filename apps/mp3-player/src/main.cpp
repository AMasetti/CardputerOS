// MP3 Player para M5Stack Cardputer / Cardputer-Adv, con menús estilo iPod.
//
// Lee los .mp3 de /music en la microSD, los agrupa por artista y disco según
// los tags ID3 y los reproduce por el parlante (o el jack en la Adv).
//
// Teclas:
//   ;  .       arriba / abajo (mantener para avanzar rápido)
//   enter  /   entrar / reproducir
//   ,  del     volver            h   menú principal
//   space      play / pausa      tab ir y volver de Now Playing
//   -  =       volumen           n p siguiente / anterior
//   s  r       shuffle / repeat  a   agregar a una playlist
//   Now Playing: , /  anterior / siguiente (mantener: retroceder / adelantar)
//                ; .  volumen     enter: ver el disco
//   Playlists:   e renombrar  x borrar
//   Playlist:    m mover      x quitar

#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include <string>
#include <vector>

#include "art.h"
#include "library.h"
#include "player.h"
#include "playlists.h"
#include "util.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// --- SD en el bus SPI de la Cardputer -------------------------------------
static constexpr int SD_SCK = 40;
static constexpr int SD_MISO = 39;
static constexpr int SD_MOSI = 14;
static constexpr int SD_CS = 12;

// --- Pantalla y estilo ------------------------------------------------------
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

static constexpr uint16_t C_BG = rgb(255, 255, 255);
static constexpr uint16_t C_TEXT = rgb(0, 0, 0);
static constexpr uint16_t C_DIM = rgb(110, 112, 118);
static constexpr uint16_t C_LINE = rgb(160, 162, 168);
static constexpr uint16_t C_HEAD1 = rgb(250, 250, 252);
static constexpr uint16_t C_HEAD2 = rgb(196, 200, 208);
static constexpr uint16_t C_SEL1 = rgb(90, 160, 245);
static constexpr uint16_t C_SEL2 = rgb(25, 95, 210);
static constexpr uint16_t C_MOVE1 = rgb(255, 180, 70);
static constexpr uint16_t C_MOVE2 = rgb(230, 120, 0);
static constexpr uint16_t C_ACCENT = rgb(25, 95, 210);
static constexpr uint16_t C_ART_BG = rgb(225, 228, 234);

static constexpr int W = 240, H = 135;
static constexpr int HEAD_H = 16;
static constexpr int ADD_H = 13;
static constexpr int ROW_H = 17;
static constexpr int ART_X = 6, ART_Y = 20;

static const lgfx::IFont *FONT = &fonts::lgfxJapanGothicP_12;
static const lgfx::IFont *FONT_BIG = &fonts::lgfxJapanGothicP_16;

static M5Canvas canvas(&M5Cardputer.Display);
static M5Canvas artSpr;
static int artAlbum = -2;
static bool artOk = false;

static Preferences prefs;

// --- Navegación ---------------------------------------------------------------
enum class V : uint8_t { Main, AddRoot, Artists, Artist, ArtistSongs, Albums, Album, Songs, Playlists, Playlist, Settings, NowPlaying };

struct View {
    View(V t = V::Main, int a = 0) : type(t), arg(a) {}
    V type;
    int arg = 0;
    int sel = 0;
    int top = 0;
};

static std::vector<View> stack;
static int addTarget = -1; // playlist que recibe temas ("+ Add Songs")
static int addDepth = 0;   // posición en el stack de la vista AddRoot
static bool moving = false; // reordenando una playlist

enum MainItem : uint8_t { M_NOW, M_PLAYLISTS, M_ARTISTS, M_ALBUMS, M_SONGS, M_SHUFFLE, M_SETTINGS };
static const char *MAIN_LABELS[] = {"Now Playing", "Playlists", "Artists", "Albums", "Songs", "Shuffle Songs", "Settings"};
static const MainItem ADD_ITEMS[] = {M_ARTISTS, M_ALBUMS, M_SONGS};

enum SettingItem : uint8_t { S_SHUFFLE, S_REPEAT, S_REBUILD, S_ABOUT, S_COUNT };

// --- Modales -------------------------------------------------------------------
enum class M : uint8_t { None, Picker, Text, Confirm };
enum Action : uint8_t { A_CREATE, A_CREATE_ADD, A_RENAME, A_DELETE, A_REBUILD };

struct Modal {
    M type = M::None;
    Action action = A_CREATE;
    int arg = 0;
    int sel = 0;
    int top = 0;
    std::string title;
    std::string text;
    std::vector<uint16_t> tracks;
};

static Modal modal;

// --- Estado de la interfaz ------------------------------------------------------
static bool dirty = true;
static bool animating = false;
static uint32_t lastDraw = 0;
static uint32_t selSince = 0;  // para el desplazamiento del texto seleccionado
static uint32_t npSince = 0;   // idem para el título en Now Playing
static std::string toastMsg;
static uint32_t toastUntil = 0;
static uint32_t volUntil = 0;
static int battery = -1;
static uint32_t prefsDue = 0;

static void markSel()
{
    selSince = millis();
    dirty = true;
}

static void toast(const std::string &msg, uint32_t ms = 1500)
{
    toastMsg = msg;
    toastUntil = millis() + ms;
    dirty = true;
}

static void savePrefsLater() { prefsDue = millis() + 1500; }

// --- Datos de cada vista ----------------------------------------------------------
static std::vector<uint16_t> scratch;
static int scratchArtist = -1;

static void mainItems(std::vector<MainItem> &out)
{
    out.clear();
    if (player::current() >= 0) out.push_back(M_NOW);
    for (MainItem m : {M_PLAYLISTS, M_ARTISTS, M_ALBUMS, M_SONGS, M_SHUFFLE, M_SETTINGS}) out.push_back(m);
}

// Lista de temas de la vista; offset = filas fijas antes del primer tema.
static const std::vector<uint16_t> *viewTracks(const View &v, int &offset)
{
    offset = 0;
    switch (v.type) {
    case V::Album: return &lib::albums[v.arg].tracks;
    case V::Songs: return &lib::songOrder;
    case V::ArtistSongs:
        if (scratchArtist != v.arg) {
            lib::artistTracks(v.arg, scratch);
            scratchArtist = v.arg;
        }
        return &scratch;
    case V::Playlist: offset = 1; return &pls::lists[v.arg].tracks;
    default: return nullptr;
    }
}

static int itemCount(const View &v)
{
    int off;
    if (auto *t = viewTracks(v, off)) return off + t->size();
    switch (v.type) {
    case V::Main: {
        std::vector<MainItem> items;
        mainItems(items);
        return items.size();
    }
    case V::AddRoot: return 3;
    case V::Artists: return lib::artists.size();
    case V::Artist: return 1 + lib::artists[v.arg].albums.size();
    case V::Albums: return lib::albums.size();
    case V::Playlists: return 1 + pls::lists.size();
    case V::Settings: return S_COUNT;
    default: return 0;
    }
}

struct Row {
    std::string text;
    std::string right;
    bool arrow = false;
    bool special = false;
    int track = -1;
};

static Row rowAt(const View &v, int i)
{
    Row r;
    char buf[16];
    int off;
    if (auto *list = viewTracks(v, off)) {
        if (i < off) {
            r.text = "+ Add Songs";
            r.special = true;
            return r;
        }
        int t = (*list)[i - off];
        r.track = t;
        r.text = lib::title(t);
        if (v.type == V::Album || v.type == V::ArtistSongs) {
            util::fmtTime(buf, sizeof(buf), lib::tracks[t].seconds);
            r.right = buf;
        } else {
            r.right = lib::artistOf(t);
        }
        return r;
    }
    switch (v.type) {
    case V::Main: {
        std::vector<MainItem> items;
        mainItems(items);
        r.text = MAIN_LABELS[items[i]];
        r.arrow = items[i] != M_SHUFFLE;
        break;
    }
    case V::AddRoot:
        r.text = MAIN_LABELS[ADD_ITEMS[i]];
        r.arrow = true;
        break;
    case V::Artists:
        r.text = lib::artistName(lib::artistOrder[i]);
        r.arrow = true;
        break;
    case V::Artist:
        r.arrow = true;
        if (i == 0) {
            r.text = "All Songs";
        } else {
            int al = lib::artists[v.arg].albums[i - 1];
            r.text = lib::albumName(al);
            if (lib::albums[al].year) r.right = std::to_string(lib::albums[al].year);
        }
        break;
    case V::Albums: {
        int al = lib::albumOrder[i];
        r.text = lib::albumName(al);
        r.right = lib::artistName(lib::albums[al].artist);
        r.arrow = true;
        break;
    }
    case V::Playlists:
        if (i == 0) {
            r.text = "+ New Playlist";
            r.special = true;
        } else {
            r.text = pls::lists[i - 1].name;
            r.right = std::to_string(pls::lists[i - 1].tracks.size());
            r.arrow = true;
        }
        break;
    case V::Settings:
        switch (i) {
        case S_SHUFFLE: r.text = "Shuffle"; r.right = player::shuffle() ? "On" : "Off"; break;
        case S_REPEAT: {
            static const char *R[] = {"Off", "All", "One"};
            r.text = "Repeat";
            r.right = R[(int)player::repeat()];
            break;
        }
        case S_REBUILD: r.text = "Rebuild Library"; break;
        case S_ABOUT: r.text = "About"; r.right = "v" APP_VERSION; break;
        }
        break;
    default: break;
    }
    return r;
}

static std::string viewTitle(const View &v)
{
    switch (v.type) {
    case V::Main: return "Music";
    case V::AddRoot: return "Add Songs";
    case V::Artists: return "Artists";
    case V::Artist:
    case V::ArtistSongs: return lib::artistName(v.arg);
    case V::Albums: return "Albums";
    case V::Album: return lib::albumName(v.arg);
    case V::Songs: return "Songs";
    case V::Playlists: return "Playlists";
    case V::Playlist: return pls::lists[v.arg].name;
    case V::Settings: return "Settings";
    case V::NowPlaying: return "Now Playing";
    }
    return "";
}

// --- Dibujo: utilidades -----------------------------------------------------------
static uint16_t lerp565(uint16_t a, uint16_t b, int num, int den)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (br - ar) * num / den, g = ag + (bg - ag) * num / den, bl = ab + (bb - ab) * num / den;
    return r << 11 | g << 5 | bl;
}

static void gradient(int x, int y, int w, int h, uint16_t c1, uint16_t c2)
{
    for (int i = 0; i < h; ++i) canvas.drawFastHLine(x, y + i, w, lerp565(c1, c2, i, h > 1 ? h - 1 : 1));
}

static void popUtf8(std::string &s)
{
    while (!s.empty()) {
        uint8_t c = s.back();
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

static std::string ellipsize(const std::string &s, int maxW)
{
    if (canvas.textWidth(s.c_str()) <= maxW) return s;
    std::string r = s;
    int dots = canvas.textWidth("...");
    while (!r.empty() && canvas.textWidth(r.c_str()) + dots > maxW) popUtf8(r);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r + "...";
}

// Texto en una caja de ancho w. Si no entra: "..." o, con scroll, se desplaza.
static void drawTextBox(const std::string &s, int x, int yMid, int w, bool scroll, uint32_t since)
{
    int tw = canvas.textWidth(s.c_str());
    if (tw <= w) {
        canvas.drawString(s.c_str(), x, yMid);
        return;
    }
    if (!scroll) {
        canvas.drawString(ellipsize(s, w).c_str(), x, yMid);
        return;
    }
    animating = true;
    const int gap = 36, speed = 32; // px/s
    const int cycle = tw + gap;
    const uint32_t pause = 1500, cycleMs = cycle * 1000 / speed;
    uint32_t t = millis() - since;
    int off = 0;
    if (t > pause) {
        uint32_t ph = (t - pause) % (cycleMs + pause);
        off = ph < cycleMs ? ph * speed / 1000 : 0;
    }
    int fh = canvas.fontHeight();
    canvas.setClipRect(x, yMid - fh / 2 - 2, w, fh + 4);
    canvas.drawString(s.c_str(), x - off, yMid);
    if (off) canvas.drawString(s.c_str(), x - off + cycle, yMid);
    canvas.clearClipRect();
}

static void iconPlay(int x, int y, uint16_t c) { canvas.fillTriangle(x, y, x, y + 8, x + 6, y + 4, c); }

static void iconPause(int x, int y, uint16_t c)
{
    canvas.fillRect(x, y, 2, 9, c);
    canvas.fillRect(x + 4, y, 2, 9, c);
}

static void iconShuffle(int x, int y, uint16_t c)
{
    canvas.drawLine(x, y + 1, x + 8, y + 7, c);
    canvas.drawLine(x, y + 7, x + 8, y + 1, c);
    canvas.fillTriangle(x + 9, y + 1, x + 6, y - 1, x + 6, y + 3, c);
    canvas.fillTriangle(x + 9, y + 7, x + 6, y + 5, x + 6, y + 9, c);
}

static void iconRepeat(int x, int y, uint16_t c, bool one)
{
    canvas.drawRoundRect(x, y + 1, 11, 8, 3, c);
    canvas.fillRect(x + 4, y, 3, 3, C_BG);
    canvas.fillTriangle(x + 6, y - 1, x + 6, y + 3, x + 9, y + 1, c);
    if (one) {
        canvas.setTextColor(c);
        canvas.drawString("1", x + 13, y + 5);
    }
}

static void iconBattery(int x, int y)
{
    if (battery < 0) return;
    uint16_t c = battery <= 20 ? rgb(220, 40, 30) : rgb(60, 170, 60);
    canvas.drawRect(x, y, 18, 9, C_TEXT);
    canvas.fillRect(x + 18, y + 2, 2, 5, C_TEXT);
    canvas.fillRect(x + 2, y + 2, std::max(1, 14 * std::min(battery, 100) / 100), 5, c);
}

static void drawNote(int x, int y, int s)
{
    gradient(x, y, s, s, rgb(236, 238, 242), rgb(200, 204, 212));
    int cx = x + s / 2, cy = y + s / 2;
    uint16_t c = rgb(150, 154, 162);
    canvas.fillEllipse(cx - 11, cy + 14, 8, 6, c);
    canvas.fillEllipse(cx + 13, cy + 10, 8, 6, c);
    canvas.fillRect(cx - 5, cy - 16, 3, 30, c);
    canvas.fillRect(cx + 19, cy - 20, 3, 30, c);
    canvas.fillTriangle(cx - 5, cy - 16, cx + 22, cy - 20, cx + 22, cy - 13, c);
    canvas.fillTriangle(cx - 5, cy - 16, cx - 5, cy - 9, cx + 22, cy - 13, c);
}

// --- Dibujo: pantallas ------------------------------------------------------------
static void drawHeader(const std::string &title)
{
    gradient(0, 0, W, HEAD_H - 1, C_HEAD1, C_HEAD2);
    canvas.drawFastHLine(0, HEAD_H - 1, W, C_LINE);
    canvas.setFont(FONT);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(ellipsize(title, 160).c_str(), W / 2, HEAD_H / 2);
    canvas.setTextDatum(middle_left);
    if (player::current() >= 0) {
        if (player::paused()) iconPause(5, 3, C_TEXT);
        else iconPlay(5, 3, C_ACCENT);
    }
    iconBattery(W - 24, 3);
}

static int listTop() { return HEAD_H + (addTarget >= 0 ? ADD_H : 0); }

static void drawList(View &v)
{
    const int n = itemCount(v);
    const int top = listTop();
    const int rows = (H - top) / ROW_H;

    if (addTarget >= 0) {
        canvas.fillRect(0, HEAD_H, W, ADD_H, rgb(225, 236, 252));
        canvas.setTextColor(C_ACCENT);
        drawTextBox("Adding to: " + pls::lists[addTarget].name, 6, HEAD_H + ADD_H / 2, W - 12, false, 0);
    }

    if (n == 0) {
        canvas.setTextColor(C_DIM);
        canvas.setTextDatum(middle_center);
        canvas.drawString(lib::tracks.empty() ? "No music in /music" : "Empty", W / 2, top + (H - top) / 2);
        canvas.setTextDatum(middle_left);
        return;
    }

    v.sel = std::max(0, std::min(v.sel, n - 1));
    if (v.sel < v.top) v.top = v.sel;
    if (v.sel >= v.top + rows) v.top = v.sel - rows + 1;
    v.top = std::max(0, std::min(v.top, std::max(0, n - rows)));

    const bool bar = n > rows;
    const int right = W - (bar ? 6 : 2);
    const int playing = player::current();

    for (int r = 0; r < rows && v.top + r < n; ++r) {
        const int i = v.top + r;
        const int y = top + r * ROW_H;
        const int mid = y + ROW_H / 2;
        const bool sel = i == v.sel;
        Row row = rowAt(v, i);

        if (sel) {
            bool mv = moving && v.type == V::Playlist && row.track >= 0;
            gradient(0, y, right, ROW_H, mv ? C_MOVE1 : C_SEL1, mv ? C_MOVE2 : C_SEL2);
        }
        int x2 = right - 4;
        if (row.track >= 0 && row.track == playing) {
            iconPlay(x2 - 6, mid - 4, sel ? C_BG : C_ACCENT);
            x2 -= 11;
        } else if (row.arrow) {
            uint16_t c = sel ? C_BG : C_DIM;
            canvas.drawLine(x2 - 4, mid - 4, x2, mid, c);
            canvas.drawLine(x2 - 4, mid + 4, x2, mid, c);
            canvas.drawLine(x2 - 5, mid - 4, x2 - 1, mid, c);
            canvas.drawLine(x2 - 5, mid + 4, x2 - 1, mid, c);
            x2 -= 11;
        }
        if (!row.right.empty()) {
            std::string rt = ellipsize(row.right, 80);
            int rw = canvas.textWidth(rt.c_str());
            canvas.setTextColor(sel ? C_BG : C_DIM);
            canvas.drawString(rt.c_str(), x2 - rw, mid);
            x2 -= rw + 8;
        }
        canvas.setTextColor(sel ? C_BG : row.special ? C_ACCENT : C_TEXT);
        drawTextBox(row.text, 6, mid, x2 - 6, sel, selSince);
    }

    if (bar) {
        int th = std::max(8, (H - top) * rows / n);
        int ty = top + (H - top - th) * v.top / std::max(1, n - rows);
        canvas.fillRect(W - 4, top, 3, H - top, rgb(230, 232, 236));
        canvas.fillRoundRect(W - 4, ty, 3, th, 1, C_LINE);
    }
}

static void drawNowPlaying()
{
    const int t = player::current();
    char buf[24];
    if (t < 0) {
        drawHeader("Now Playing");
        canvas.setTextColor(C_DIM);
        canvas.setTextDatum(middle_center);
        canvas.drawString("Nothing playing", W / 2, 66);
        canvas.drawString("del: back", W / 2, 86);
        canvas.setTextDatum(middle_left);
        return;
    }
    snprintf(buf, sizeof(buf), "%d of %d", player::queuePos() + 1, player::queueSize());
    drawHeader(buf);

    const lib::Track &tr = lib::tracks[t];
    if (artAlbum != tr.album) {
        artOk = art::load(tr.album, artSpr);
        artAlbum = tr.album;
    }
    if (artOk) artSpr.pushSprite(&canvas, ART_X, ART_Y);
    else drawNote(ART_X, ART_Y, art::SIZE);
    canvas.drawRect(ART_X - 1, ART_Y - 1, art::SIZE + 2, art::SIZE + 2, C_LINE);

    const int x = ART_X + art::SIZE + 9, w = W - x - 5;
    canvas.setFont(FONT_BIG);
    canvas.setTextColor(C_TEXT);
    drawTextBox(lib::title(t), x, ART_Y + 10, w, true, npSince);
    canvas.setFont(FONT);
    canvas.setTextColor(rgb(40, 40, 46));
    drawTextBox(lib::artistOf(t), x, ART_Y + 32, w, false, 0);
    canvas.setTextColor(C_DIM);
    drawTextBox(lib::albumName(tr.album), x, ART_Y + 48, w, false, 0);

    const int yIcons = ART_Y + 64;
    if (millis() < volUntil) {
        canvas.fillTriangle(x, yIcons + 2, x + 4, yIcons - 1, x + 4, yIcons + 9, C_DIM);
        canvas.fillRect(x, yIcons + 2, 2, 5, C_DIM);
        int bw = w - 12;
        canvas.drawRoundRect(x + 10, yIcons + 1, bw, 7, 3, C_LINE);
        canvas.fillRoundRect(x + 10, yIcons + 1, std::max(7, bw * player::volume() / 255), 7, 3, C_ACCENT);
    } else {
        int ix = x;
        if (lib::albums[tr.album].year) {
            canvas.drawString(std::to_string(lib::albums[tr.album].year).c_str(), ix, yIcons + 4);
            ix += canvas.textWidth("0000") + 8;
        }
        if (player::shuffle()) iconShuffle(ix, yIcons, C_ACCENT), ix += 16;
        if (player::repeat() != player::Repeat::Off) iconRepeat(ix, yIcons, C_ACCENT, player::repeat() == player::Repeat::One);
    }

    // Barra de avance
    const uint32_t el = player::elapsed(), len = tr.seconds;
    const int by = ART_Y + art::SIZE + 6, bx = 6, bw = W - 12;
    canvas.fillRoundRect(bx, by, bw, 6, 3, rgb(218, 221, 227));
    if (len) {
        int fw = std::min<int>(bw, (uint64_t)bw * std::min(el, len) / len);
        if (fw > 0) {
            canvas.fillRoundRect(bx, by, std::max(6, fw), 6, 3, C_SEL2);
            canvas.drawFastHLine(bx + 2, by + 1, std::max(2, fw - 4), C_SEL1);
        }
    }
    canvas.setTextColor(C_TEXT);
    util::fmtTime(buf, sizeof(buf), el);
    canvas.drawString(buf, bx, by + 14);
    if (len) {
        buf[0] = '-';
        util::fmtTime(buf + 1, sizeof(buf) - 1, len > el ? len - el : 0);
    } else {
        strcpy(buf, "--:--");
    }
    canvas.drawString(buf, bx + bw - canvas.textWidth(buf), by + 14);
    if (player::paused()) {
        canvas.setTextColor(C_DIM);
        canvas.setTextDatum(middle_center);
        canvas.drawString("Paused", W / 2, by + 14);
        canvas.setTextDatum(middle_left);
    }
}

static void modalBox(int x, int y, int w, int h, const std::string &title)
{
    canvas.fillRoundRect(x + 2, y + 2, w, h, 5, rgb(120, 122, 128));
    canvas.fillRoundRect(x, y, w, h, 5, C_BG);
    canvas.drawRoundRect(x, y, w, h, 5, C_LINE);
    canvas.setTextColor(C_TEXT);
    canvas.setTextDatum(middle_center);
    canvas.drawString(ellipsize(title, w - 16).c_str(), x + w / 2, y + 10);
    canvas.setTextDatum(middle_left);
    canvas.drawFastHLine(x + 6, y + 19, w - 12, rgb(220, 222, 228));
}

static void drawModal()
{
    const int x = 18, y = 14, w = W - 36, h = H - 28;
    modalBox(x, y, w, h, modal.title);
    canvas.setFont(FONT);

    if (modal.type == M::Picker) {
        const int rowH = 15, top = y + 22, rows = (h - 26) / rowH;
        const int n = 1 + pls::lists.size();
        modal.sel = std::max(0, std::min(modal.sel, n - 1));
        if (modal.sel < modal.top) modal.top = modal.sel;
        if (modal.sel >= modal.top + rows) modal.top = modal.sel - rows + 1;
        for (int r = 0; r < rows && modal.top + r < n; ++r) {
            int i = modal.top + r, ry = top + r * rowH;
            bool sel = i == modal.sel;
            if (sel) gradient(x + 3, ry, w - 6, rowH, C_SEL1, C_SEL2);
            canvas.setTextColor(sel ? C_BG : i == 0 ? C_ACCENT : C_TEXT);
            drawTextBox(i == 0 ? "+ New Playlist" : pls::lists[i - 1].name, x + 9, ry + rowH / 2, w - 18, sel, selSince);
        }
    } else if (modal.type == M::Text) {
        const int fy = y + 34;
        canvas.drawRoundRect(x + 8, fy, w - 16, 20, 3, C_ACCENT);
        std::string shown = modal.text;
        while (!shown.empty() && canvas.textWidth(shown.c_str()) > w - 28) shown.erase(0, 1);
        canvas.setTextColor(C_TEXT);
        canvas.drawString(shown.c_str(), x + 13, fy + 10);
        if (millis() / 500 % 2) canvas.drawFastVLine(x + 14 + canvas.textWidth(shown.c_str()), fy + 4, 12, C_TEXT);
        animating = true;
        canvas.setTextColor(C_DIM);
        canvas.setTextDatum(middle_center);
        canvas.drawString("enter: OK    `: cancel", x + w / 2, y + h - 14);
        canvas.setTextDatum(middle_left);
    } else if (modal.type == M::Confirm) {
        canvas.setTextColor(C_TEXT);
        canvas.setTextDatum(middle_center);
        canvas.drawString(ellipsize(modal.text, w - 16).c_str(), x + w / 2, y + 44);
        canvas.setTextColor(C_DIM);
        canvas.drawString("enter: Yes    del: No", x + w / 2, y + h - 14);
        canvas.setTextDatum(middle_left);
    }
}

static void drawToast()
{
    std::string msg = ellipsize(toastMsg, W - 40);
    int tw = canvas.textWidth(msg.c_str()) + 20;
    int x = (W - tw) / 2, y = H - 26;
    canvas.fillRoundRect(x, y, tw, 20, 6, rgb(40, 42, 48));
    canvas.setTextColor(C_BG);
    canvas.setTextDatum(middle_center);
    canvas.drawString(msg.c_str(), W / 2, y + 10);
    canvas.setTextDatum(middle_left);
}

static void draw()
{
    animating = false;
    canvas.fillScreen(C_BG);
    canvas.setFont(FONT);
    canvas.setTextDatum(middle_left);

    View &v = stack.back();
    if (v.type == V::NowPlaying) {
        drawNowPlaying();
    } else {
        drawHeader(viewTitle(v));
        drawList(v);
    }
    if (modal.type != M::None) drawModal();

    uint32_t now = millis();
    if (now < toastUntil) {
        drawToast();
    } else if (now < volUntil && v.type != V::NowPlaying) {
        char b[16];
        snprintf(b, sizeof(b), "Volume %d%%", player::volume() * 100 / 255);
        toastMsg = b;
        drawToast();
    }
    canvas.pushSprite(0, 0);
    lastDraw = now;
    dirty = false;
}

// --- Acciones -------------------------------------------------------------------------
static void push(V type, int arg = 0)
{
    stack.push_back(View(type, arg));
    moving = false;
    if (type == V::NowPlaying) npSince = millis();
    markSel();
}

static void goBack()
{
    if (stack.size() <= 1) return;
    stack.pop_back();
    moving = false;
    if (addTarget >= 0 && (int)stack.size() <= addDepth) addTarget = -1;
    markSel();
}

static void goHome()
{
    stack.resize(1);
    addTarget = -1;
    moving = false;
    markSel();
}

static void toggleNowPlaying()
{
    if (stack.back().type == V::NowPlaying) goBack();
    else push(V::NowPlaying);
}

// Tras crear/borrar/renombrar, las playlists se reordenan: corrige los índices
// guardados en el stack.
template <typename F>
static void changePlaylists(F op)
{
    std::vector<std::string> names(stack.size());
    for (size_t k = 0; k < stack.size(); ++k)
        if (stack[k].type == V::Playlist) names[k] = pls::lists[stack[k].arg].name;
    std::string addName = addTarget >= 0 ? pls::lists[addTarget].name : "";
    op();
    for (size_t k = 1; k < stack.size(); ++k) {
        if (stack[k].type != V::Playlist) continue;
        int ni = pls::find(names[k]);
        if (ni < 0) {
            stack.resize(k);
            break;
        }
        stack[k].arg = ni;
    }
    if (addTarget >= 0) {
        addTarget = pls::find(addName);
        if (addTarget < 0 && (int)stack.size() > addDepth) stack.resize(addDepth);
    }
    dirty = true;
}

static std::string addedMsg(size_t n, int pl)
{
    return (n == 1 ? std::string("Added to ") : "Added " + std::to_string(n) + " songs to ") + pls::lists[pl].name;
}

static void openText(const std::string &title, Action a, const std::string &initial = "", int arg = 0)
{
    modal.type = M::Text;
    modal.title = title;
    modal.action = a;
    modal.text = initial;
    modal.arg = arg;
    dirty = true;
}

static void openConfirm(const std::string &title, const std::string &msg, Action a, int arg = 0)
{
    modal.type = M::Confirm;
    modal.title = title;
    modal.text = msg;
    modal.action = a;
    modal.arg = arg;
    dirty = true;
}

static void addTracks(const std::vector<uint16_t> &tracks)
{
    if (tracks.empty()) return;
    if (addTarget >= 0) {
        pls::add(addTarget, tracks);
        toast(addedMsg(tracks.size(), addTarget));
        return;
    }
    modal = Modal();
    modal.type = M::Picker;
    modal.title = "Add to Playlist";
    modal.tracks = tracks;
    modal.sel = pls::lists.empty() ? 0 : 1;
    markSel();
}

static void progressScreen(const char *msg, int done, int total)
{
    canvas.fillScreen(C_BG);
    canvas.setFont(FONT);
    canvas.setTextDatum(middle_left);
    drawHeader("Music");
    canvas.setTextColor(C_TEXT);
    canvas.setTextDatum(middle_center);
    canvas.drawString(msg, W / 2, 56);
    if (total > 0) {
        canvas.fillRoundRect(30, 72, W - 60, 8, 4, rgb(218, 221, 227));
        canvas.fillRoundRect(30, 72, std::max(8, (W - 60) * done / total), 8, 4, C_SEL2);
        char b[24];
        snprintf(b, sizeof(b), "%d / %d", done, total);
        canvas.setTextColor(C_DIM);
        canvas.drawString(b, W / 2, 94);
    }
    canvas.setTextDatum(middle_left);
    canvas.pushSprite(0, 0);
}

static void loadLibrary(bool rebuild)
{
    if (rebuild) {
        player::stop();
        art::clearCache();
    }
    lib::load(rebuild, progressScreen);
    pls::loadAll();
    scratchArtist = -1;
    artAlbum = -2;
}

static void runModalAction()
{
    Modal m = modal;
    modal = Modal();
    dirty = true;
    switch (m.action) {
    case A_CREATE:
    case A_CREATE_ADD: {
        int i = -1;
        changePlaylists([&] { i = pls::create(m.text); });
        if (i < 0) {
            toast(pls::cleanName(m.text).empty() ? "Name is empty" : "Name already used");
            return;
        }
        if (m.action == A_CREATE_ADD) {
            pls::add(i, m.tracks);
            toast(addedMsg(m.tracks.size(), i));
        } else {
            View &v = stack.back();
            if (v.type == V::Playlists) v.sel = i + 1;
            push(V::Playlist, i);
        }
        break;
    }
    case A_RENAME: {
        int i = -1;
        changePlaylists([&] { i = pls::rename(m.arg, m.text); });
        if (i < 0) toast("Name already used");
        else if (stack.back().type == V::Playlists) stack.back().sel = i + 1;
        break;
    }
    case A_DELETE:
        changePlaylists([&] { pls::remove(m.arg); });
        toast("Playlist deleted");
        break;
    case A_REBUILD: {
        bool sh = player::shuffle();
        loadLibrary(true);
        player::setShuffle(sh);
        goHome();
        toast(std::to_string(lib::tracks.size()) + " songs");
        break;
    }
    }
}

static void activate()
{
    View &v = stack.back();
    const int i = v.sel;
    if (i >= itemCount(v)) return;

    int off;
    if (auto *list = viewTracks(v, off)) {
        if (i < off) { // "+ Add Songs"
            addTarget = v.arg;
            push(V::AddRoot);
            addDepth = stack.size() - 1;
            return;
        }
        int t = (*list)[i - off];
        if (addTarget >= 0) {
            addTracks({(uint16_t)t});
        } else {
            std::vector<uint16_t> q = *list;
            player::play(q, i - off);
            push(V::NowPlaying);
        }
        return;
    }

    switch (v.type) {
    case V::Main:
    case V::AddRoot: {
        MainItem item;
        if (v.type == V::Main) {
            std::vector<MainItem> items;
            mainItems(items);
            item = items[i];
        } else {
            item = ADD_ITEMS[i];
        }
        switch (item) {
        case M_NOW: push(V::NowPlaying); break;
        case M_PLAYLISTS: push(V::Playlists); break;
        case M_ARTISTS: push(V::Artists); break;
        case M_ALBUMS: push(V::Albums); break;
        case M_SONGS: push(V::Songs); break;
        case M_SETTINGS: push(V::Settings); break;
        case M_SHUFFLE:
            if (lib::tracks.empty()) break;
            player::setShuffle(true);
            savePrefsLater();
            player::play(lib::songOrder, esp_random() % lib::songOrder.size());
            push(V::NowPlaying);
            break;
        }
        break;
    }
    case V::Artists: push(V::Artist, lib::artistOrder[i]); break;
    case V::Artist:
        if (i == 0) push(V::ArtistSongs, v.arg);
        else push(V::Album, lib::artists[v.arg].albums[i - 1]);
        break;
    case V::Albums: push(V::Album, lib::albumOrder[i]); break;
    case V::Playlists:
        if (i == 0) openText("New Playlist", A_CREATE);
        else push(V::Playlist, i - 1);
        break;
    case V::Settings:
        switch (i) {
        case S_SHUFFLE: player::setShuffle(!player::shuffle()); savePrefsLater(); break;
        case S_REPEAT: player::setRepeat((player::Repeat)(((int)player::repeat() + 1) % 3)); savePrefsLater(); break;
        case S_REBUILD: openConfirm("Rebuild Library", "Read all tags again?", A_REBUILD); break;
        case S_ABOUT:
            toast(std::to_string(lib::tracks.size()) + " songs, " + std::to_string(lib::albums.size()) + " albums", 2500);
            break;
        }
        dirty = true;
        break;
    default: break;
    }
}

static void addSelected()
{
    View &v = stack.back();
    std::vector<uint16_t> t;
    int off;
    if (v.type == V::NowPlaying) {
        if (player::current() >= 0) t.push_back(player::current());
    } else if (auto *list = viewTracks(v, off)) {
        if (v.sel >= off && v.sel < off + (int)list->size()) t.push_back((*list)[v.sel - off]);
    } else if (v.type == V::Albums && v.sel < (int)lib::albums.size()) {
        t = lib::albums[lib::albumOrder[v.sel]].tracks;
    } else if (v.type == V::Artists && v.sel < (int)lib::artists.size()) {
        lib::artistTracks(lib::artistOrder[v.sel], t);
    } else if (v.type == V::Artist) {
        if (v.sel == 0) lib::artistTracks(v.arg, t);
        else t = lib::albums[lib::artists[v.arg].albums[v.sel - 1]].tracks;
    }
    addTracks(t);
}

static void changeVolume(int delta)
{
    player::setVolume(std::max(0, std::min(255, player::volume() + delta)));
    volUntil = millis() + 1500;
    toastUntil = 0;
    savePrefsLater();
    dirty = true;
}

// --- Teclado ------------------------------------------------------------------------
static constexpr char K_ENTER = '\n', K_DEL = '\b', K_TAB = '\t';
static std::vector<char> held;
static char repKey = 0;
static uint32_t repStart = 0, repLast = 0;
static char seekKey = 0;
static uint32_t seekStart = 0, seekLast = 0;
static bool seeked = false;

static bool isBack(char c) { return c == K_DEL || c == '`' || c == ','; }

static void moveSel(int d)
{
    View &v = stack.back();
    int n = itemCount(v);
    if (!n) return;
    if (moving && v.type == V::Playlist) {
        auto &tr = pls::lists[v.arg].tracks;
        int a = v.sel - 1, b = a + d;
        if (a >= 0 && b >= 0 && b < (int)tr.size()) {
            std::swap(tr[a], tr[b]);
            v.sel += d;
        }
    } else {
        v.sel = std::max(0, std::min(n - 1, v.sel + d));
    }
    markSel();
}

static void modalKey(char c)
{
    dirty = true;
    switch (modal.type) {
    case M::Picker:
        if (c == ';') modal.sel = std::max(0, modal.sel - 1), markSel();
        else if (c == '.') modal.sel = std::min((int)pls::lists.size(), modal.sel + 1), markSel();
        else if (isBack(c)) modal = Modal();
        else if (c == K_ENTER || c == '/') {
            if (modal.sel == 0) {
                std::vector<uint16_t> t = modal.tracks;
                openText("New Playlist", A_CREATE_ADD);
                modal.tracks = t;
            } else {
                int pl = modal.sel - 1;
                pls::add(pl, modal.tracks);
                toast(addedMsg(modal.tracks.size(), pl));
                modal = Modal();
            }
        }
        break;
    case M::Text:
        if (c == K_ENTER) runModalAction();
        else if (c == '`') modal = Modal();
        else if (c == K_DEL) popUtf8(modal.text);
        else if (c >= 32 && c < 127 && modal.text.size() < 40) modal.text += c;
        break;
    case M::Confirm:
        if (c == K_ENTER || c == 'y' || c == 'Y') runModalAction();
        else if (c == K_DEL || c == '`' || c == 'n' || c == 'N') modal = Modal();
        break;
    default: break;
    }
}

static void onPress(char raw)
{
    if (modal.type != M::None) {
        modalKey(raw);
        if (raw == K_DEL) {
            repKey = raw;
            repStart = repLast = millis();
        }
        return;
    }
    char c = (raw >= 'A' && raw <= 'Z') ? raw + ('a' - 'A') : raw;
    View &v = stack.back();
    const bool np = v.type == V::NowPlaying;
    dirty = true;

    // Reordenando una playlist: solo mover y terminar.
    if (moving) {
        if (c == ';' || c == '.') {
            moveSel(c == ';' ? -1 : 1);
            repKey = c;
            repStart = repLast = millis();
        } else if (c == K_ENTER || c == 'm' || isBack(c)) {
            moving = false;
            pls::save(v.arg);
        }
        return;
    }

    switch (c) {
    case ' ': player::togglePause(); return;
    case '-':
    case '=':
        changeVolume(c == '-' ? -12 : 12);
        repKey = c;
        repStart = repLast = millis();
        return;
    case K_TAB: toggleNowPlaying(); return;
    case 'n': player::next(); return;
    case 'p': player::prev(); return;
    case 's':
        player::setShuffle(!player::shuffle());
        savePrefsLater();
        toast(player::shuffle() ? "Shuffle on" : "Shuffle off");
        return;
    case 'r': {
        player::setRepeat((player::Repeat)(((int)player::repeat() + 1) % 3));
        savePrefsLater();
        static const char *R[] = {"Repeat off", "Repeat all", "Repeat one"};
        toast(R[(int)player::repeat()]);
        return;
    }
    case 'a': addSelected(); return;
    case 'h': goHome(); return;
    }

    if (np) {
        if (c == ';' || c == '.') {
            changeVolume(c == ';' ? 12 : -12);
            repKey = c;
            repStart = repLast = millis();
        } else if (c == ',' || c == '/') {
            seekKey = c;
            seekStart = millis();
            seeked = false;
        } else if (c == K_DEL || c == '`') {
            goBack();
        } else if (c == K_ENTER && player::current() >= 0) {
            int al = lib::tracks[player::current()].album;
            push(V::Album, al);
            auto &tr = lib::albums[al].tracks;
            stack.back().sel = std::find(tr.begin(), tr.end(), player::current()) - tr.begin();
        }
        return;
    }

    if (c == ';' || c == '.') {
        moveSel(c == ';' ? -1 : 1);
        repKey = c;
        repStart = repLast = millis();
    } else if (c == K_ENTER || c == '/') {
        activate();
    } else if (isBack(c)) {
        goBack();
    } else if (v.type == V::Playlists && v.sel > 0) {
        int pl = v.sel - 1;
        if (c == 'e') openText("Rename Playlist", A_RENAME, pls::lists[pl].name, pl);
        else if (c == 'x') openConfirm("Delete Playlist", "Delete \"" + pls::lists[pl].name + "\"?", A_DELETE, pl);
    } else if (v.type == V::Playlist && v.sel > 0) {
        auto &tr = pls::lists[v.arg].tracks;
        if (c == 'm') {
            moving = true;
        } else if (c == 'x' && v.sel - 1 < (int)tr.size()) {
            tr.erase(tr.begin() + v.sel - 1);
            pls::save(v.arg);
            v.sel = std::min(v.sel, (int)tr.size());
            toast("Removed");
        }
    }
}

static void onRelease(char c)
{
    if (c == repKey) repKey = 0;
    if (c == seekKey) {
        seekKey = 0;
        if (!seeked && stack.back().type == V::NowPlaying) {
            if (c == ',') player::prev();
            else player::next();
            npSince = millis();
        }
        dirty = true;
    }
}

static void handleKeys()
{
    if (M5Cardputer.Keyboard.isChange()) {
        std::vector<char> now;
        if (M5Cardputer.Keyboard.isPressed()) {
            auto st = M5Cardputer.Keyboard.keysState();
            now = st.word;
            if (st.enter) now.push_back(K_ENTER);
            if (st.del) now.push_back(K_DEL);
            if (st.tab) now.push_back(K_TAB);
        }
        for (char c : held)
            if (std::find(now.begin(), now.end(), c) == now.end()) onRelease(c);
        for (char c : now)
            if (std::find(held.begin(), held.end(), c) == held.end()) onPress(c);
        held = now;
    }

    const uint32_t t = millis();
    if (repKey && t - repStart > 380) {
        uint32_t every = t - repStart > 2000 ? 25 : 70;
        if (repKey == '-' || repKey == '=' || repKey == K_DEL || stack.back().type == V::NowPlaying) every = 90;
        if (t - repLast >= every) {
            repLast = t;
            char c = repKey;
            if (c == '-' || c == '=') changeVolume(c == '-' ? -12 : 12);
            else if (modal.type != M::None) { if (c == K_DEL) modalKey(c); }
            else if (c != ';' && c != '.') repKey = 0;
            else if (stack.back().type == V::NowPlaying) changeVolume(c == ';' ? 12 : -12);
            else moveSel(c == ';' ? -1 : 1);
        }
    }
    if (seekKey && t - seekStart > 500 && t - seekLast > 300) {
        seekLast = t;
        seeked = true;
        int step = t - seekStart > 3500 ? 20 : 5;
        player::seek(seekKey == ',' ? -step : step);
        dirty = true;
    }
}

// --- Arduino --------------------------------------------------------------------------
void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    canvas.setColorDepth(16);
    canvas.createSprite(W, H);
    artSpr.setColorDepth(16);
    artSpr.createSprite(art::SIZE, art::SIZE);
    M5Cardputer.Speaker.begin();

    prefs.begin("mp3player", false);
    uint8_t vol = prefs.getUChar("vol", 128);
    bool sh = prefs.getBool("shuf", false);
    uint8_t rp = prefs.getUChar("rep", 0);
    player::begin(vol, sh, (player::Repeat)std::min<uint8_t>(rp, 2));

    stack.push_back(View(V::Main));
    progressScreen("Opening microSD...", 0, 0);
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    while (!SD.begin(SD_CS, SPI, 25000000)) {
        progressScreen("Insert a microSD and press enter", 0, 0);
        do {
            M5Cardputer.update();
            delay(20);
        } while (!M5Cardputer.Keyboard.keysState().enter);
    }
    loadLibrary(false);
    battery = M5Cardputer.Power.getBatteryLevel();
    markSel();
}

void loop()
{
    M5Cardputer.update();
    handleKeys();
    player::update();

    static int lastTrack = -1;
    static bool lastPaused = false;
    int cur = player::current();
    if (cur != lastTrack || player::paused() != lastPaused) {
        if (cur != lastTrack) npSince = millis();
        lastTrack = cur;
        lastPaused = player::paused();
        dirty = true;
    }

    uint32_t now = millis();
    static uint32_t lastBattery = 0;
    if (now - lastBattery > 10000) {
        battery = M5Cardputer.Power.getBatteryLevel();
        lastBattery = now;
    }
    if (prefsDue && now > prefsDue) {
        prefs.putUChar("vol", player::volume());
        prefs.putBool("shuf", player::shuffle());
        prefs.putUChar("rep", (uint8_t)player::repeat());
        prefsDue = 0;
    }

    bool np = stack.back().type == V::NowPlaying;
    uint32_t every = animating ? 33 : (np || now < toastUntil + 300 || now < volUntil + 300) ? 250 : 1000;
    if (dirty || now - lastDraw >= every) draw();
    delay(2);
}
