// Wrapper de NimBLE en modo scan-only (observer).
//
// El rol scan-only está forzado en dos capas, no solo acá:
//   1. platformio.ini deshabilita los roles PERIPHERAL/BROADCASTER/CENTRAL
//      a nivel de compilación (CONFIG_BT_NIMBLE_ROLE_*_DISABLED) -> ni este
//      código ni ningún otro puede anunciar ni conectar aunque lo intente.
//   2. Este wrapper nunca llama a startAdvertising() ni a ningún método de
//      conexión, por diseño.
// Ver docs/BLE_SENTINEL_SPEC.md sección "Auto-ocultamiento".
#pragma once

#include <Arduino.h>
#include <vector>

namespace blescanner {

// Un avistamiento crudo, ya extraído del adv packet, listo para pasarle a
// fingerprint::compute() y signaturedb::matchByCompanyId().
struct RawSighting {
    String mac;
    int8_t rssi = 0;
    uint16_t companyId = 0xFFFF; // 0xFFFF = sin manufacturer data
    // Byte inmediatamente después del company ID en el manufacturer data.
    // Varios fabricantes (Apple es el caso que nos importa: un mismo
    // company ID 0x004C cubre iPhones, AirPods, Watch, Find My...) lo usan
    // como "sub-protocolo" -- sin esto no se puede distinguir un AirTag/
    // accesorio Find My (type 0x12) de cualquier otro dispositivo Apple.
    // 0 si no hay manufacturer data o tiene menos de 3 bytes.
    uint8_t mfgTypeByte = 0;
    uint8_t payloadLen = 0;
    int8_t txPowerDbm = INT8_MIN;
    std::vector<uint16_t> serviceUuids16; // ordenados
    // ASTM Remote ID (drones) -- UUID 0xFFFA asignado por el Bluetooth SIG
    // a "ASTM Remote ID" (assigned_numbers/uuids/sdo_uuids.yaml del
    // registro público). Viaja en el AD type Service Data (0x16), NO en la
    // lista general de Service UUIDs (0x02/0x03) que ya se extrae arriba
    // -- son dos estructuras AD distintas, por eso necesita su propio campo
    // en vez de reusar serviceUuids16.
    bool remoteIdService = false;
    String localName;                     // puede venir vacío
};

void begin();

// Escaneo continuo (no por duración fija): arranca una vez y sigue hasta
// stop(). Igual que shell-rc pero sin filtro de nombre y sin timeout.
void start();
void stop();
bool scanning();

// Llamar en loop(): drena lo que juntó el callback de NimBLE (que corre en
// su propia tarea) hacia `out`, protegido por mutex. Devuelve cuántos
// avistamientos nuevos copió.
size_t poll(std::vector<RawSighting> &out);

} // namespace blescanner
