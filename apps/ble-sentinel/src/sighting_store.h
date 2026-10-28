// Registro de avistamientos por fingerprint, con log a SD.
//
// v1 sin RTC (decisión tomada en la conversación: se agrega más adelante
// por Grove/I2C). Todo timestamp es `millis()` relativo al boot de ESTA
// sesión, no hora de pared. Eso implica:
//   - "recurrente, mismo lugar" funciona (compara dentro de la sesión).
//   - "recurrente entre sesiones separadas por un apagado" NO funciona
//     todavía: cada boot es una sesión nueva sin forma de saber cuánto
//     tiempo pasó entre una y otra. Queda explícitamente fuera de alcance
//     hasta que el RTC esté.
// Cuando llegue el RTC, se suma un campo `wallClockMs` opcional (poblado
// solo si `rtcAvailable`) sin tocar lo que ya hay.
#pragma once

#include <Arduino.h>
#include <vector>
#include "signature_db.h"

namespace sightingstore {

struct Record {
    uint64_t fingerprintHash = 0;
    String lastMac;
    int8_t lastRssi = 0;
    uint32_t firstSeenMs = 0;  // relativo a begin() de esta sesión
    uint32_t lastSeenMs = 0;
    uint16_t timesSeen = 0;
    uint8_t macChanges = 0;    // cuántas MAC distintas vimos para este fingerprint
    signaturedb::Category category = signaturedb::Category::Unknown;
    bool confidenceLow = false;

    // Crudo del último advertising packet, para la pantalla de detalle --
    // resolve() en main.cpp ya consume esto para categorizar, pero antes
    // se perdía: sin guardarlo acá no había forma de mostrar "fabricante"
    // o "nombre anunciado" después del hecho.
    uint16_t lastCompanyId = 0xFFFF; // 0xFFFF = sin manufacturer data
    uint8_t lastMfgTypeByte = 0;      // ver ble_scanner.h / signature_db.h matchFindMyNetwork()
    int8_t lastTxPowerDbm = INT8_MIN; // INT8_MIN = no declarado
    uint8_t lastPayloadLen = 0;
    String lastLocalName; // vacío si el dispositivo no lo anunció

    // Marcado a mano por el usuario ("este es mío, no me alertes si
    // parece seguirme" -- caso típico: tu propio AirTag, que por
    // definición está siempre con vos y por eso dispara ConvoyAnalyzer
    // igual que lo haría un tracker ajeno). Persiste a SD, ver setTrusted().
    bool trusted = false;
};

// Resultado de registrar un avistamiento, para que el caller sepa si vale
// la pena alertar/loggear o es solo una repetición reciente.
enum class SightingKind : uint8_t {
    New,         // primera vez que vemos este fingerprint en esta sesión
    MacRotated,  // mismo fingerprint, MAC distinta, delta compatible con rotación
    Update,      // mismo fingerprint y MAC, solo actualiza RSSI/timestamp
};

// Inicializa la sesión: monta /sentinel en SD (si `sdReady`, resultado de
// que SD.begin() haya funcionado en main.cpp) y asigna session_id
// incremental persistido en /sentinel/session.txt. Si no hay SD, sigue
// funcionando solo en RAM (degradado, sin log persistente).
void begin(bool sdReady);

uint32_t sessionId();
bool sdAvailable();

// Log a SD de eventos (New/MacRotated) -- arranca APAGADO por defecto,
// aunque haya tarjeta: grabar no debería ser el default implícito solo
// porque hay una SD puesta, el usuario lo prende cuando de verdad quiere
// dejar constancia. Toggleable en caliente con una tecla (ver main.cpp).
// No afecta a `records` ni al resto del store -- es solo el log a
// archivo; apagarlo no borra ni oculta nada de lo que ya se vio en esta
// sesión, y un dispositivo puede dejar de verse en la UI (filtrado por
// "activo") sin que eso tenga nada que ver con esto: mientras está
// prendido, se loggea todo lo que pasa por record(), visible en la UI o
// no.
void setLoggingEnabled(bool on);
bool loggingEnabled();

// Reinicia la sesión de prueba SIN reiniciar el dispositivo (tecla `r` en
// main.cpp) -- limpia `records` (todo lo visto hasta ahora en RAM) para
// empezar una corrida de campo nueva. Si hay SD, también cierra el log de
// historial actual y abre uno nuevo bajo un session_id incrementado
// (mismo mecanismo que begin(), para que el archivo de esta corrida no se
// mezcle con la anterior) -- sin SD, `sessionId()` igual se incrementa en
// RAM nomás, solo para que el HUD distinga una corrida de la siguiente.
// `trusted` NO se toca (ver setTrusted()): lo marcado como propio no
// debería tener que remarcarse en cada reset de prueba.
void resetSession();

// Entrada para record(): identidad resuelta (fingerprint/categoría, ya
// decidida por el caller) + los campos crudos del advertising packet que
// vale la pena guardar para la pantalla de detalle.
struct SightingInput {
    uint64_t fingerprintHash;
    String mac;
    int8_t rssi;
    signaturedb::Category category;
    bool confidenceLow;
    uint16_t companyId;
    uint8_t mfgTypeByte;
    int8_t txPowerDbm;
    uint8_t payloadLen;
    String localName;
};

// Registra/actualiza un avistamiento. Devuelve el tipo de evento y, por
// out-param, el Record actualizado.
SightingKind record(const SightingInput &in, Record &out);

size_t count();
const Record *byIndex(size_t i); // para listar en UI, ordenado por lastSeenMs desc
const Record *byFingerprint(uint64_t fingerprintHash); // para seguir un dispositivo puntual (radar)

// Marca/desmarca un dispositivo como propio -- ver comentario de
// `Record::trusted`. Persiste a SD en /sentinel/trusted.txt (si hay
// tarjeta) para sobrevivir un reinicio; sin SD queda solo en RAM para
// esta sesión, igual que el resto del estado degradado.
void setTrusted(uint64_t fingerprintHash, bool trusted);
bool isTrusted(uint64_t fingerprintHash);

} // namespace sightingstore
