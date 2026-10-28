// Detección de "convoy" / seguimiento sostenido: distingue "hay un
// dispositivo cerca" de "este dispositivo me está siguiendo".
//
// El Cardputer base no tiene GPS, así que un "contexto" (lugar) es lo que
// el usuario marca a mano con markNewContext() -- no hay forma automática
// de saber que uno se movió. Si más adelante se agrega un módulo GPS, lo
// natural es que dispare markNewContext() solo cuando el desplazamiento
// supera un umbral, sin tocar el resto de esta API.
//
// Ver docs/BLE_SENTINEL_SPEC.md sección 2.
#pragma once

#include <Arduino.h>

namespace convoyanalyzer {

void begin();

// "Cambié de lugar": todo lo que se vea a partir de ahora cuenta para un
// contexto nuevo. Atado a una tecla en main.cpp.
void markNewContext();
uint32_t currentContext();
uint32_t contextCount(); // cuántos contextos se marcaron en esta sesión (arranca en 1)

// Alimenta el analizador con cada avistamiento -- se llama desde
// processSightings en main.cpp para cualquier kind (New/Update/MacRotated),
// ya que lo que importa acá es "en qué contexto se vio este fingerprint",
// no si cambió de MAC.
void onSighting(uint64_t fingerprintHash);

constexpr uint8_t CONVOY_THRESHOLD = 60;

// Puntaje 0-100 (ver fórmula en el .cpp). >= CONVOY_THRESHOLD se considera
// "posible seguimiento".
//
// Pendiente (no suma puntos todavía): correlación entre la tendencia de
// RSSI y el movimiento propio del usuario. Necesitaría una señal de
// movimiento (acelerómetro o GPS) que v1 no integra -- ver spec.
uint8_t score(uint64_t fingerprintHash);
bool isSuspect(uint64_t fingerprintHash);

} // namespace convoyanalyzer
