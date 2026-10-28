#include "alert_engine.h"
#include <M5Cardputer.h>

namespace alertengine {

namespace {

// Defaults v1 acordados en la conversación: sospechoso a volumen pleno,
// auricular a volumen bajo, confiado en silencio.
const Profile PROFILES[] = {
    /* SuspiciousDefault  */ {100, 1200, 160, 2, 60000},
    /* HeadsetLowPriority */ {35, 800, 160, 1, 60000},
    /* Silent             */ {0, 0, 0, 0, 0},
    /* ConvoySuspect      */ {100, 2200, 120, 4, 30000}, // más agudo, más beeps, cooldown más corto
};

const Profile &profileFor(signaturedb::AlertProfile p)
{
    return PROFILES[static_cast<uint8_t>(p)];
}

// Cooldown por fingerprint: tabla fija, se pisa la entrada más vieja cuando
// se llena. No necesitamos más que "no repetir seguido", así que un mapa
// exacto sin límite de tamaño sería sobrekill para un microcontrolador.
constexpr size_t COOLDOWN_SLOTS = 64;
struct CooldownSlot {
    uint64_t hash = 0;
    uint32_t lastTriggerMs = 0;
    bool used = false;
};
CooldownSlot cooldowns[COOLDOWN_SLOTS];
size_t nextSlot = 0;

bool withinCooldown(uint64_t hash, uint32_t cooldownMs, uint32_t nowMs)
{
    for (auto &slot : cooldowns) {
        if (slot.used && slot.hash == hash) {
            return (nowMs - slot.lastTriggerMs) < cooldownMs;
        }
    }
    return false;
}

void recordTrigger(uint64_t hash, uint32_t nowMs)
{
    for (auto &slot : cooldowns) {
        if (slot.used && slot.hash == hash) {
            slot.lastTriggerMs = nowMs;
            return;
        }
    }
    // Asignación campo a campo, no brace-init: CooldownSlot tiene default
    // member initializers, lo que la saca de ser agregado bajo el -std que
    // usa este framework -- `CooldownSlot{a,b,c}` buscaría un constructor
    // de 3 argumentos en vez de inicializar por posición.
    CooldownSlot &slot = cooldowns[nextSlot];
    slot.hash = hash;
    slot.lastTriggerMs = nowMs;
    slot.used = true;
    nextSlot = (nextSlot + 1) % COOLDOWN_SLOTS;
}

// Estado del patrón de beeps en curso, avanzado por update().
struct PendingPattern {
    bool active = false;
    Profile profile;
    uint8_t beepsLeft = 0;
    uint32_t nextActionMs = 0;
    bool toneOn = false;
};
PendingPattern pending;

// Arranca en mute: el audio lo activa explícitamente G0 (ver main.cpp), no
// queda sonando por defecto apenas se prende la app.
bool g_enabled = false;
uint8_t g_masterVolume = 180; // 0-255, mismo rango que M5Cardputer.Speaker

void startPattern(const Profile &p)
{
    pending.active = true;
    pending.profile = p;
    pending.beepsLeft = p.beepCount;
    pending.nextActionMs = millis();
    pending.toneOn = false;
}

} // namespace

void begin()
{
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(g_masterVolume);
}

void setEnabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }

void setMasterVolume(uint8_t v)
{
    g_masterVolume = v;
    M5Cardputer.Speaker.setVolume(g_masterVolume);
}
uint8_t masterVolume() { return g_masterVolume; }

void trigger(signaturedb::AlertProfile profileId, uint64_t fingerprintHash)
{
    if (!g_enabled) return;

    const Profile &p = profileFor(profileId);
    if (p.volumePct == 0 || p.beepCount == 0) return; // perfil Silent

    // El cooldown es por (dispositivo, perfil), no solo por dispositivo:
    // sin esto, una alerta de ConvoySuspect para un fingerprint que ya
    // disparó SuspiciousDefault hace poco quedaría bloqueada por el
    // cooldown de esa OTRA alerta, aunque sean cosas distintas. profileId
    // cabe en pocos bits, así que mezclarlo en los bits altos del hash no
    // genera colisiones prácticas entre dispositivos reales.
    uint64_t cooldownKey = fingerprintHash ^ (static_cast<uint64_t>(profileId) << 60);

    uint32_t now = millis();
    if (withinCooldown(cooldownKey, p.cooldownMs, now)) return;
    recordTrigger(cooldownKey, now);

    startPattern(p);
}

void update()
{
    if (!pending.active) return;

    uint32_t now = millis();
    if (now < pending.nextActionMs) return;

    const Profile &p = pending.profile;

    if (!pending.toneOn) {
        if (pending.beepsLeft == 0) {
            pending.active = false;
            return;
        }
        // Volumen efectivo = master * volumePct del perfil. setVolume toma
        // 0-255, igual que el resto de la app (ver tuner/mp3-player).
        uint8_t effectiveVolume = (uint16_t)g_masterVolume * p.volumePct / 100;
        M5Cardputer.Speaker.setVolume(effectiveVolume);
        M5Cardputer.Speaker.tone(p.toneFreqHz, p.toneDurationMs);
        pending.toneOn = true;
        pending.nextActionMs = now + p.toneDurationMs;
        pending.beepsLeft--;
    } else {
        // Pausa entre beeps, mismo largo que el tono para un patrón parejo.
        pending.toneOn = false;
        pending.nextActionMs = now + p.toneDurationMs;
        if (pending.beepsLeft == 0) {
            // Restaurar volumen master para que no quede pisado para otros usos.
            M5Cardputer.Speaker.setVolume(g_masterVolume);
        }
    }
}

} // namespace alertengine
