#include "fingerprint.h"

namespace fingerprint {

namespace {

// FNV-1a 64 bits. No es criptográfico (el spec original hablaba de SHA1
// truncado) pero para este uso -diferenciar dispositivos, no resistir un
// atacante activo- alcanza y evita sumar una lib de hashing solo para esto.
constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

void mix(uint64_t &h, uint8_t byte)
{
    h ^= byte;
    h *= FNV_PRIME;
}

void mixU16(uint64_t &h, uint16_t v)
{
    mix(h, v & 0xFF);
    mix(h, (v >> 8) & 0xFF);
}

// TX power varía unos dB entre lecturas del mismo dispositivo por ruido de
// RF; lo agrupamos en buckets de 4dBm para que esa variación no rompa el
// match (ver spec: "TX power declarado... Estabilidad: Media-alta").
int8_t txPowerBucket(int8_t dbm)
{
    if (dbm == INT8_MIN) return INT8_MIN; // ausente, no bucketizar
    return dbm / 4;
}

} // namespace

Result compute(const AdvInput &in)
{
    Result r;
    uint64_t h = FNV_OFFSET;

    mixU16(h, in.companyId);
    mix(h, in.mfgTypeByte);
    mix(h, in.payloadLen);
    mix(h, static_cast<uint8_t>(txPowerBucket(in.txPowerDbm)));
    mix(h, in.adTypesPresentMask);
    mix(h, in.remoteIdService ? 1 : 0);

    // serviceUuids16 ya viene ordenado por el caller: mismo set -> mismo
    // hash sin importar el orden en que NimBLE los reportó.
    for (uint16_t uuid : in.serviceUuids16) {
        mixU16(h, uuid);
    }

    r.hash = h;

    // Payload pobre: sin manufacturer data y sin service UUIDs, solo queda
    // payloadLen + txPower como entrada -> fingerprint débil, muchos
    // dispositivos distintos pueden colisionar en el mismo hash.
    // Remote ID es la excepción: un match de `remoteIdService` es una señal
    // tan específica como un company ID (es un UUID puntual, no un default
    // "no hay nada acá"), así que por sí sola ya basta para no bajar la
    // confianza aunque no haya manufacturer data ni otros service UUIDs --
    // de hecho los Remote ID típicos no tienen manufacturer data.
    r.confidenceLow = (in.companyId == 0xFFFF) && in.serviceUuids16.empty() && !in.remoteIdService;

    return r;
}

bool isPlausibleMacRotation(uint32_t deltaMs)
{
    // Ventana amplia (5-25 min) para tolerar jitter del intervalo real de
    // rotación, que varía por fabricante/firmware.
    constexpr uint32_t MIN_MS = 5UL * 60UL * 1000UL;
    constexpr uint32_t MAX_MS = 25UL * 60UL * 1000UL;
    return deltaMs >= MIN_MS && deltaMs <= MAX_MS;
}

} // namespace fingerprint
