#include "convoy_analyzer.h"
#include <vector>

namespace convoyanalyzer {

namespace {

// Tope de contextos distintos que seguimos por dispositivo. 8 alcanza de
// sobra para una sesión real (¿cuántos lugares distintos marcás en un
// día?) y evita que un fingerprint visto en loop crezca sin límite.
constexpr size_t MAX_CONTEXTS_PER_DEVICE = 8;

// Sin default member initializers a propósito: son agregados de verdad
// bajo cualquier -std, así que el push_back({a,b,c}) de abajo no pisa el
// mismo bug que ya salió en alert_engine.cpp.
struct ContextSighting {
    uint32_t contextId;
    uint32_t firstSeenMs;
    uint32_t lastSeenMs;
};

struct ConvoyState {
    uint64_t fingerprintHash;
    std::vector<ContextSighting> contexts;
};

std::vector<ConvoyState> states;
uint32_t g_currentContext = 1;
uint32_t g_contextCount = 1;

ConvoyState *find(uint64_t hash)
{
    for (auto &s : states) {
        if (s.fingerprintHash == hash) return &s;
    }
    return nullptr;
}

} // namespace

void begin()
{
    states.clear();
    states.reserve(32);
    g_currentContext = 1;
    g_contextCount = 1;
}

void markNewContext()
{
    g_currentContext++;
    g_contextCount++;
}

uint32_t currentContext() { return g_currentContext; }
uint32_t contextCount() { return g_contextCount; }

void onSighting(uint64_t fingerprintHash)
{
    uint32_t now = millis();
    ConvoyState *s = find(fingerprintHash);
    if (!s) {
        states.push_back({fingerprintHash, {}});
        s = &states.back();
    }

    for (auto &c : s->contexts) {
        if (c.contextId == g_currentContext) {
            c.lastSeenMs = now;
            return;
        }
    }
    if (s->contexts.size() >= MAX_CONTEXTS_PER_DEVICE) {
        s->contexts.erase(s->contexts.begin()); // descarta el contexto más viejo
    }
    s->contexts.push_back({g_currentContext, now, now});
}

uint8_t score(uint64_t fingerprintHash)
{
    ConvoyState *s = find(fingerprintHash);
    if (!s || s->contexts.empty()) return 0;

    uint16_t sc = 0;
    size_t distinctContexts = s->contexts.size();

    // Visto en 3 o más lugares distintos marcados a mano -> no es "lo cruce
    // una vez en la calle".
    if (distinctContexts >= 3) sc += 40;

    // Promedio de cuánto duró cada avistamiento dentro de su contexto. Un
    // dispositivo que aparece y desaparece rápido pesa menos que uno que se
    // queda un rato sostenido en cada lugar.
    uint32_t totalDurationMs = 0;
    for (const auto &c : s->contexts) totalDurationMs += (c.lastSeenMs - c.firstSeenMs);
    uint32_t avgDurationMs = totalDurationMs / distinctContexts;
    constexpr uint32_t SUSTAINED_MS = 5UL * 60UL * 1000UL; // 5 min, ver spec
    if (avgDurationMs > SUSTAINED_MS) sc += 20;

    // Ventana temporal total entre el primer y el último avistamiento,
    // cruzando contextos.
    uint32_t firstMs = s->contexts.front().firstSeenMs;
    uint32_t lastMs = s->contexts.back().lastSeenMs;
    constexpr uint32_t LONG_SPAN_MS = 60UL * 60UL * 1000UL; // 1h, ver spec
    if (lastMs - firstMs > LONG_SPAN_MS) sc += 10;

    // Pendiente (0 puntos en v1): correlación RSSI/movimiento propio, ver
    // signature del header. Con los 3 factores de arriba el máximo posible
    // hoy es 70 -- igual alcanza para cruzar CONVOY_THRESHOLD=60 con
    // "3+ contextos" solo, o con "3+ contextos" + cualquiera de los otros
    // dos. Un solo factor aislado nunca dispara la alerta.

    return sc > 100 ? 100 : static_cast<uint8_t>(sc);
}

bool isSuspect(uint64_t fingerprintHash)
{
    return score(fingerprintHash) >= CONVOY_THRESHOLD;
}

} // namespace convoyanalyzer
