#include "player.h"

#include <M5Cardputer.h>
#include <SD.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>

#include <AudioFileSourceSD.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>

#include "library.h"

namespace player {

// --- Salida: puente ESP8266Audio -> M5.Speaker ------------------------------
// Tres buffers en secuencia; nunca bloquea: si la cola del parlante está
// llena devuelve false y el decodificador reintenta más tarde.
class SpeakerOut : public AudioOutput {
  public:
    explicit SpeakerOut(uint8_t ch) : _ch(ch) {}

    bool begin() override { return true; }

    bool ConsumeSample(int16_t sample[2]) override
    {
        if (_idx >= BUF_LEN && !push()) return false;
        _buf[_bank][_idx++] = sample[0];
        _buf[_bank][_idx++] = sample[1];
        frames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Al terminar el tema: manda lo que quedó en el buffer.
    void drain()
    {
        while (_idx && !push()) vTaskDelay(1);
    }

    bool stop() override { return true; }

    uint32_t rate() const { return hertz; }

    void reset()
    {
        _idx = 0;
        frames = 0;
    }

    std::atomic<uint32_t> frames{0};

  private:
    bool push()
    {
        if (M5Cardputer.Speaker.isPlaying(_ch) >= 2) return false;
        M5Cardputer.Speaker.playRaw(_buf[_bank], _idx, hertz, true, 1, _ch);
        _bank = (_bank + 1) % BANKS;
        _idx = 0;
        return true;
    }

    static constexpr size_t BUF_LEN = 1536; // 768 muestras estéreo, ~17 ms
    static constexpr size_t BANKS = 3;
    uint8_t _ch;
    int16_t _buf[BANKS][BUF_LEN];
    size_t _bank = 0;
    size_t _idx = 0;
};

static constexpr uint8_t CH = 0;

static std::mutex mx;
static SpeakerOut *out = nullptr;
static AudioFileSourceSD *src = nullptr;
static AudioFileSourceBuffer *buff = nullptr;
static AudioGeneratorMP3 *gen = nullptr;

static std::atomic<int> curTrack{-1};
static std::atomic<bool> isPaused{false};
static std::atomic<bool> ended{false};
static uint32_t baseSec = 0; // segundo donde arrancó (después de un seek)

static std::vector<uint16_t> queue;
static std::vector<uint16_t> order; // índices de queue en el orden de reproducción
static int pos = 0;
static bool shuf = false;
static Repeat rep = Repeat::Off;
static uint8_t vol = 128;

// --- Tarea de audio -----------------------------------------------------------
static void audioTask(void *)
{
    for (;;) {
        bool progressed = false;
        {
            std::lock_guard<std::mutex> lk(mx);
            if (gen && !isPaused && gen->isRunning()) {
                uint32_t before = out->frames;
                if (!gen->loop()) {
                    out->drain();
                    gen->stop();
                    ended = true;
                } else {
                    progressed = out->frames != before;
                }
            }
        }
        // Siempre cede el núcleo un rato para no disparar el watchdog del idle.
        vTaskDelay(progressed ? 1 : 3);
    }
}

// --- Control (con el mutex tomado) ----------------------------------------------
// El constructor de AudioGeneratorMP3 no inicializa los punteros que libera el
// destructor: se crea sobre memoria en cero para que borrarlo siempre sea seguro.
static AudioGeneratorMP3 *newGen()
{
    void *mem = calloc(1, sizeof(AudioGeneratorMP3));
    return mem ? new (mem) AudioGeneratorMP3() : nullptr;
}

static void closeLocked(bool cut)
{
    if (gen) {
        if (gen->isRunning()) gen->stop();
        gen->~AudioGeneratorMP3();
        free(gen);
        gen = nullptr;
    }
    delete buff;
    buff = nullptr;
    delete src;
    src = nullptr;
    if (cut) M5Cardputer.Speaker.stop(CH);
    out->reset();
}

static bool startTrack(int t, uint32_t startSec, bool cut)
{
    std::lock_guard<std::mutex> lk(mx);
    closeLocked(cut);
    ended = false;
    if (t < 0 || t >= (int)lib::tracks.size()) {
        curTrack = -1;
        return false;
    }
    char path[400];
    lib::trackPath(t, path, sizeof(path));
    src = new AudioFileSourceSD(path);
    if (!src->isOpen()) {
        closeLocked(cut);
        curTrack = -1;
        return false;
    }
    const lib::Track &tr = lib::tracks[t];
    uint32_t size = src->getSize();
    uint32_t off = tr.audioOff < size ? tr.audioOff : 0;
    if (startSec && tr.seconds) off += (uint64_t)(size - off) * startSec / tr.seconds;
    src->seek(off, SEEK_SET);
    buff = new AudioFileSourceBuffer(src, 8192);
    gen = newGen();
    baseSec = startSec;
    curTrack = t;
    if (!gen || !gen->begin(buff, out)) {
        closeLocked(cut);
        curTrack = -1;
        return false;
    }
    return true;
}

static void stopAll()
{
    std::lock_guard<std::mutex> lk(mx);
    closeLocked(true);
    curTrack = -1;
    isPaused = false;
}

static void buildOrder(int keepFirst)
{
    order.resize(queue.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (shuf && order.size() > 1) {
        for (size_t i = order.size() - 1; i > 0; --i) std::swap(order[i], order[esp_random() % (i + 1)]);
        if (keepFirst >= 0) {
            auto it = std::find(order.begin(), order.end(), keepFirst);
            std::iter_swap(order.begin(), it);
        }
        pos = 0;
    } else {
        pos = keepFirst >= 0 ? keepFirst : 0;
    }
}

static void playPos(bool cut)
{
    // Si un tema no abre, prueba con los siguientes.
    for (size_t tries = 0; tries < order.size(); ++tries) {
        if (startTrack(queue[order[pos]], 0, cut)) {
            isPaused = false;
            return;
        }
        if (pos + 1 >= (int)order.size()) break;
        ++pos;
    }
    stopAll();
}

// --- API --------------------------------------------------------------------------
void begin(uint8_t volume, bool shuffle, Repeat repeat)
{
    vol = volume;
    shuf = shuffle;
    rep = repeat;
    M5Cardputer.Speaker.setVolume(vol);
    out = new SpeakerOut(CH);
    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
}

void update()
{
    if (!ended) return;
    ended = false;
    if (curTrack < 0) return;
    if (rep == Repeat::One) {
        startTrack(curTrack, 0, false);
    } else if (pos + 1 < (int)order.size()) {
        ++pos;
        playPos(false);
    } else if (rep == Repeat::All && !order.empty()) {
        if (shuf) buildOrder(-1);
        pos = 0;
        playPos(false);
    } else {
        stopAll();
    }
}

void play(const std::vector<uint16_t> &q, int index)
{
    if (q.empty()) return;
    queue = q;
    buildOrder(std::max(0, std::min(index, (int)q.size() - 1)));
    playPos(true);
}

void stop()
{
    stopAll();
    queue.clear();
    order.clear();
    pos = 0;
}

void togglePause()
{
    if (curTrack < 0) {
        if (!order.empty()) playPos(true);
        return;
    }
    isPaused = !isPaused;
}

void next()
{
    if (order.empty()) return;
    if (pos + 1 < (int)order.size()) {
        ++pos;
    } else {
        if (shuf) buildOrder(-1);
        pos = 0;
    }
    playPos(true);
}

void prev()
{
    if (order.empty()) return;
    if (elapsed() > 3 || pos == 0) {
        if (curTrack >= 0) startTrack(curTrack, 0, true), isPaused = false;
        else playPos(true);
        return;
    }
    --pos;
    playPos(true);
}

void seek(int deltaSec)
{
    int t = curTrack;
    if (t < 0) return;
    int len = lib::tracks[t].seconds;
    int target = (int)elapsed() + deltaSec;
    if (len && target >= len) {
        next();
        return;
    }
    bool wasPaused = isPaused;
    startTrack(t, std::max(0, target), true);
    isPaused = wasPaused;
}

int current() { return curTrack; }
bool paused() { return isPaused; }

uint32_t elapsed()
{
    if (!out || curTrack < 0) return 0;
    return baseSec + out->frames / std::max<uint32_t>(8000, out->rate());
}

int queuePos() { return pos; }
int queueSize() { return order.size(); }

bool shuffle() { return shuf; }

void setShuffle(bool on)
{
    if (on == shuf) return;
    shuf = on;
    int keep = order.empty() ? -1 : order[pos];
    if (!queue.empty()) buildOrder(keep);
}

Repeat repeat() { return rep; }
void setRepeat(Repeat r) { rep = r; }
uint8_t volume() { return vol; }

void setVolume(uint8_t v)
{
    vol = v;
    M5Cardputer.Speaker.setVolume(vol);
}

} // namespace player
