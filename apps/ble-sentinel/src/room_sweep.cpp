#include "room_sweep.h"
#include <M5Cardputer.h>
#include "alert_engine.h"

namespace roomsweep {

namespace {

// Historial crudo para el sparkline -- ventana corta a propósito, es para
// "¿cómo viene la señal ahora?", no para tendencias de largo plazo (eso es
// trabajo de ConvoyAnalyzer). Se muestra sin filtrar porque el ruido real
// es información (ver header): tapar eso en el gráfico sería mentir sobre
// lo estable que es la señal.
constexpr size_t HISTORY_LEN = 8;
int8_t g_history[HISTORY_LEN];
size_t g_historyCount = 0;

// Dos EMA (promedio móvil exponencial) sobre el mismo RSSI, con distinta
// velocidad de reacción -- el clásico "cruce rápido/lento" para detectar
// tendencia sin que un solo paquete ruidoso la dispare. alpha más alto =
// reacciona más rápido = pesa más el paquete de ahora.
constexpr float EMA_FAST_ALPHA = 0.35f;
constexpr float EMA_SLOW_ALPHA = 0.06f;
// Zona muerta sobre la DIFERENCIA entre las dos EMA, no sobre el RSSI
// crudo -- al estar ambas filtradas, una diferencia sostenida de 2.5dBm
// entre ellas ya es señal real, no los 10-15dBm de ruido normal de RF.
constexpr float TREND_DEAD_ZONE_DBM = 2.5f;
float g_emaFast = 0.0f;
float g_emaSlow = 0.0f;
bool g_emaInit = false; // false hasta la primera muestra, para no arrancar en 0

// EMA propia para la burbuja -- deliberadamente más lenta que g_emaFast:
// la burbuja es la UI principal ahora, así que prioriza que se mueva
// suave y con confianza por sobre reaccionar rápido. No comparte estado
// con g_emaFast/g_emaSlow (esas quedan solo para trend()/debug) para que
// ajustar una no afecte a la otra.
// Rango real observado en hardware (Cardputer + ESP32-S3), no el rango
// teórico de BLE -- la antena/TX power de este equipo nunca llega a los
// -40dBm "de libro" ni al lado del dispositivo, y -95dBm es un piso
// demasiado generoso para "lejos" en la práctica (usuario reportó -60dBm
// al lado, -80dBm lejos). Con el rango teórico la burbuja nunca se
// llenaba ni se vaciaba del todo. Un poco de margen a cada lado (5dBm)
// para no pegar en el tope/piso apenas se mueve un poco.
constexpr float EMA_BUBBLE_ALPHA = 0.05f;
constexpr float BUBBLE_RSSI_MIN = -85.0f; // fracción 0.0 (lejos)
constexpr float BUBBLE_RSSI_MAX = -55.0f; // fracción 1.0 (al lado)
float g_emaBubble = 0.0f;
bool g_bubbleInit = false;

uint64_t g_followedHash = 0;
bool g_following = false;

bool g_toneMode = false;
bool g_toneActive = false;
uint16_t g_lastToneFreq = 0;
uint32_t g_lastToneUpdateMs = 0;
constexpr uint32_t TONE_UPDATE_INTERVAL_MS = 120;
constexpr uint16_t TONE_FREQ_MIN = 300;  // BUBBLE_RSSI_MIN, lejos
constexpr uint16_t TONE_FREQ_MAX = 1800; // BUBBLE_RSSI_MAX, cerca

void silenceTone()
{
    if (g_toneActive) {
        M5Cardputer.Speaker.stop();
        g_toneActive = false;
    }
}

} // namespace

void begin()
{
    g_following = false;
    g_followedHash = 0;
    g_historyCount = 0;
    g_emaInit = false;
    g_bubbleInit = false;
    g_toneMode = false;
    silenceTone();
}

void follow(uint64_t fingerprintHash)
{
    g_followedHash = fingerprintHash;
    g_following = true;
    g_historyCount = 0;
    g_emaInit = false;
    g_bubbleInit = false;
}

void stop()
{
    g_following = false;
    g_toneMode = false; // cada vez que se vuelve a seguir algo, arranca en silencio
    silenceTone();
}

bool isFollowing() { return g_following; }
uint64_t followedHash() { return g_followedHash; }

void sample(uint64_t fingerprintHash, int8_t rssi)
{
    if (!g_following || fingerprintHash != g_followedHash) return;

    if (g_historyCount < HISTORY_LEN) {
        g_history[g_historyCount++] = rssi;
    } else {
        // Buffer lleno: shiftea. HISTORY_LEN es chico (8), este costo es
        // insignificante frente al ritmo real de advertising.
        for (size_t i = 1; i < HISTORY_LEN; i++) g_history[i - 1] = g_history[i];
        g_history[HISTORY_LEN - 1] = rssi;
    }

    if (!g_emaInit) {
        // Primera muestra: arrancar ambas EMA en el valor real, no en 0 --
        // si no, tardarían varios paquetes en "subir" desde cero y la
        // tendencia inicial saldría falsa.
        g_emaFast = g_emaSlow = static_cast<float>(rssi);
        g_emaInit = true;
    } else {
        g_emaFast = EMA_FAST_ALPHA * rssi + (1.0f - EMA_FAST_ALPHA) * g_emaFast;
        g_emaSlow = EMA_SLOW_ALPHA * rssi + (1.0f - EMA_SLOW_ALPHA) * g_emaSlow;
    }

    if (!g_bubbleInit) {
        g_emaBubble = static_cast<float>(rssi);
        g_bubbleInit = true;
    } else {
        g_emaBubble = EMA_BUBBLE_ALPHA * rssi + (1.0f - EMA_BUBBLE_ALPHA) * g_emaBubble;
    }
}

int8_t smoothedRssi()
{
    if (!g_emaInit) return 0;
    return static_cast<int8_t>(g_emaFast + (g_emaFast >= 0 ? 0.5f : -0.5f)); // redondeo
}

Trend trend()
{
    // Pedimos algunas muestras antes de afirmar nada, igual que v1, pero
    // ahora la decisión real la toma el cruce de EMA, no un split de la
    // ventana cruda -- eso es lo que hacía que un solo paquete ruidoso
    // volteara el resultado.
    if (!g_emaInit || g_historyCount < 4) return Trend::Stable;

    // RSSI menos negativo = señal más fuerte = más cerca. Si la EMA rápida
    // (reacciona a lo reciente) está por encima de la lenta (el promedio
    // de más tiempo), la señal viene subiendo -> acercándose.
    float delta = g_emaFast - g_emaSlow;
    if (delta > TREND_DEAD_ZONE_DBM) return Trend::Closer;
    if (delta < -TREND_DEAD_ZONE_DBM) return Trend::Farther;
    return Trend::Stable;
}

float proximityFraction()
{
    if (!g_bubbleInit) return 0.0f;
    float clamped = g_emaBubble < BUBBLE_RSSI_MIN ? BUBBLE_RSSI_MIN
                     : (g_emaBubble > BUBBLE_RSSI_MAX ? BUBBLE_RSSI_MAX : g_emaBubble);
    return (clamped - BUBBLE_RSSI_MIN) / (BUBBLE_RSSI_MAX - BUBBLE_RSSI_MIN);
}

size_t historyCount() { return g_historyCount; }

int8_t historyAt(size_t i)
{
    if (i >= g_historyCount) return 0;
    return g_history[i];
}

void setToneMode(bool on)
{
    g_toneMode = on;
    if (!on) silenceTone();
}

bool toneMode() { return g_toneMode; }

void updateTone()
{
    if (!g_toneMode || !g_following || !alertengine::enabled()) {
        silenceTone();
        return;
    }

    uint32_t now = millis();
    if (now - g_lastToneUpdateMs < TONE_UPDATE_INTERVAL_MS) return;
    g_lastToneUpdateMs = now;

    // Mismo rango calibrado que la burbuja (BUBBLE_RSSI_MIN/MAX) -- si no,
    // el tono y la burbuja contarían dos historias distintas de "qué tan
    // cerca" para la misma señal.
    int rssiMin = static_cast<int>(BUBBLE_RSSI_MIN);
    int rssiMax = static_cast<int>(BUBBLE_RSSI_MAX);
    int rssi = smoothedRssi();
    int rssiClamped = rssi < rssiMin ? rssiMin : (rssi > rssiMax ? rssiMax : rssi);
    uint16_t freq = TONE_FREQ_MIN + static_cast<uint16_t>(
        (rssiClamped - rssiMin) * (TONE_FREQ_MAX - TONE_FREQ_MIN) / (rssiMax - rssiMin));

    // Solo retrigger el tono si cambió lo suficiente -- reiniciar la forma
    // de onda cada 120ms con la misma frecuencia solo produce clicks.
    int delta = static_cast<int>(freq) - static_cast<int>(g_lastToneFreq);
    if (!g_toneActive || delta > 15 || delta < -15) {
        M5Cardputer.Speaker.setVolume(alertengine::masterVolume());
        M5Cardputer.Speaker.tone(freq, 0); // duración 0 = continuo hasta stop()
        g_lastToneFreq = freq;
        g_toneActive = true;
    }
}

} // namespace roomsweep
