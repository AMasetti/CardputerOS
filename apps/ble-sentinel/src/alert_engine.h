// Alertas de audio por categoría de dispositivo.
//
// v1 trae 2 perfiles con sonido (sospechoso por defecto y auricular/headset
// a bajo volumen, tal como se definió en la conversación) más uno silencioso
// para baseline/confiado. Pensado para sumar perfiles nuevos sin tocar la
// lógica: trigger() ya recibe la categoría y resuelve el perfil con una
// tabla, no con if/else por tipo de dispositivo.
#pragma once

#include <Arduino.h>
#include "signature_db.h"

namespace alertengine {

struct Profile {
    uint8_t volumePct;      // relativo al volumen master (0-100)
    uint16_t toneFreqHz;
    uint16_t toneDurationMs;
    uint8_t beepCount;
    uint32_t cooldownMs;    // no repetir la misma alerta antes de este tiempo
};

void begin();

// Llamar en loop(): avanza el patrón de beeps en curso (para perfiles con
// beepCount > 1, que necesitan silencio entre tonos sin bloquear el resto
// de la app).
void update();

// Habilitado/deshabilitado general desde la UI. Si está apagado, trigger()
// no emite nada (pero no afecta las alertas visuales, que van aparte).
void setEnabled(bool on);
bool enabled();

// 0-255, al estilo M5Cardputer.Speaker.setVolume(). Es el tope sobre el que
// se aplica volumePct de cada perfil.
void setMasterVolume(uint8_t v);
uint8_t masterVolume();

// Dispara la alerta correspondiente a `profile` para `fingerprintHash`,
// respetando el cooldown por dispositivo (evita que un tracker fijo beepee
// sin parar mientras sigue a la vista). No bloquea: encola el tono y vuelve.
void trigger(signaturedb::AlertProfile profile, uint64_t fingerprintHash);

} // namespace alertengine
