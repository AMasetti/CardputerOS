// Fingerprint de un dispositivo BLE más allá de su MAC address.
//
// La MAC rota (privacidad BLE estándar, cada ~15 min en la mayoría de los
// dispositivos de consumo) pero el resto del paquete de advertising suele
// quedar estable por dispositivo/firmware: company ID, forma del payload,
// TX power declarado, set de service UUIDs. Combinando esos campos armamos
// un hash que sigue siendo el mismo aunque la MAC cambie, para poder decir
// "este es el mismo dispositivo que vi hace 10 min con otra MAC" en vez de
// tratarlo como uno nuevo cada vez.
//
// Ver docs/BLE_SENTINEL_SPEC.md sección 1.
#pragma once

#include <Arduino.h>
#include <vector>

namespace fingerprint {

// Entrada normalizada que arma el BLEScanner a partir de un adv packet.
// companyId = 0xFFFF si no hay manufacturer data.
// txPowerDbm = INT8_MIN si el campo no vino en el paquete.
struct AdvInput {
    uint16_t companyId = 0xFFFF;
    // Byte de sub-protocolo dentro del manufacturer data (ver ble_scanner.h):
    // sin esto, todos los dispositivos Apple (company ID 0x004C) comparten
    // fingerprint salvo que varíen en otro campo -- un AirTag y un AirPods
    // podrían colisionar.
    uint8_t mfgTypeByte = 0;
    uint8_t payloadLen = 0;
    int8_t txPowerDbm = INT8_MIN;
    std::vector<uint16_t> serviceUuids16; // ya ordenados por el caller
    uint8_t adTypesPresentMask = 0;       // bitmask liviano de qué AD types trae
    bool remoteIdService = false;         // ASTM Remote ID (ver ble_scanner.h)
};

// Resultado del cálculo. confidenceLow indica que el payload era muy pobre
// (sin manufacturer data ni service UUIDs) y el fingerprint es menos fiable
// para linkear avistamientos entre sí.
struct Result {
    uint64_t hash = 0;
    bool confidenceLow = false;
};

Result compute(const AdvInput &in);

// true si dos avistamientos separados en el tiempo por `deltaMs` son
// compatibles con una rotación de MAC típica de privacidad BLE (~10-20 min).
// Útil para marcar `mac_rotation_confirmed` cuando dos sightings comparten
// fingerprint pero llegaron con MAC distinta.
bool isPlausibleMacRotation(uint32_t deltaMs);

} // namespace fingerprint
