#include "ble_scanner.h"
#include <NimBLEDevice.h>
#include <mutex>
#include <algorithm>

namespace blescanner {

namespace {

// Buffer circular simple de paquetes crudos recibidos desde la tarea de
// NimBLE; el loop principal los drena con poll(). Tamaño generoso porque en
// una zona densa (evento, subte, calle céntrica) puede haber decenas de
// dispositivos anunciando a la vez.
constexpr size_t QUEUE_CAP = 96;
std::mutex queueLock;
std::vector<RawSighting> queue;
bool g_scanning = false;

std::vector<uint16_t> extractServiceUuids16(NimBLEAdvertisedDevice *d)
{
    std::vector<uint16_t> out;
    size_t n = d->getServiceUUIDCount();
    for (size_t i = 0; i < n; i++) {
        NimBLEUUID uuid = d->getServiceUUID(i);
        if (uuid.bitSize() == 16) {
            // ble_uuid16_t.value -- acceso directo al tipo nativo de NimBLE,
            // mismo patrón que usa la lib para comparar UUIDs 16-bit.
            out.push_back(uuid.getNative()->u16.value);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *d) override
    {
        RawSighting s;
        s.mac = d->getAddress().toString().c_str();
        s.rssi = static_cast<int8_t>(d->getRSSI());

        if (d->haveManufacturerData()) {
            std::string mfg = d->getManufacturerData();
            if (mfg.size() >= 2) {
                // Company ID va little-endian en los primeros 2 bytes, por
                // spec de Bluetooth (AD type 0xFF).
                s.companyId = static_cast<uint8_t>(mfg[0]) |
                               (static_cast<uint8_t>(mfg[1]) << 8);
            }
            if (mfg.size() >= 3) {
                s.mfgTypeByte = static_cast<uint8_t>(mfg[2]);
            }
        }

        s.txPowerDbm = d->haveTXPower() ? d->getTXPower() : INT8_MIN;
        s.payloadLen = d->getPayloadLength();
        s.serviceUuids16 = extractServiceUuids16(d);

        // ASTM Remote ID (ver ble_scanner.h): Service Data (AD 0x16), no la
        // lista de Service UUIDs -- hay que mirar getServiceData*(), no
        // getServiceUUID*(). Solo nos importa si está presente el UUID
        // 0xFFFA, no decodificamos el mensaje ODID (Basic ID/Location/
        // Self-ID/etc.) todavía -- mismo nivel de profundidad que el resto
        // de las firmas v1 (presencia, no contenido).
        if (d->haveServiceData()) {
            size_t sdCount = d->getServiceDataCount();
            for (size_t i = 0; i < sdCount; i++) {
                NimBLEUUID uuid = d->getServiceDataUUID(i);
                if (uuid.bitSize() == 16 && uuid.getNative()->u16.value == 0xFFFA) {
                    s.remoteIdService = true;
                    break;
                }
            }
        }

        if (d->haveName()) s.localName = d->getName().c_str();

        std::lock_guard<std::mutex> lock(queueLock);
        if (queue.size() < QUEUE_CAP) queue.push_back(s);
        // Si se llenó, se descartan nuevos hasta el próximo poll() -- mejor
        // perder algunos paquetes de más que bloquear la tarea de NimBLE.
    }
};
ScanCallbacks scanCallbacks;

} // namespace

void begin()
{
    NimBLEDevice::init("");
    // Nombre vacío: no hay advertising de todas formas (rol deshabilitado en
    // build_flags), pero lo dejamos explícito para que quede claro que este
    // dispositivo no se identifica a sí mismo de ninguna forma.
}

void start()
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true); // duplicados: así vemos RSSI actualizado
    scan->setActiveScan(true); // pedimos scan response también (más datos por dispositivo)
    scan->setInterval(100);
    scan->setWindow(99);
    // duration=0 -> continuo hasta stop(); is_continue=false para reiniciar limpio.
    g_scanning = scan->start(0, nullptr, false);
}

void stop()
{
    NimBLEDevice::getScan()->stop();
    g_scanning = false;
}

bool scanning() { return g_scanning; }

size_t poll(std::vector<RawSighting> &out)
{
    std::lock_guard<std::mutex> lock(queueLock);
    if (queue.empty()) return 0;
    size_t n = queue.size();
    out.insert(out.end(), queue.begin(), queue.end());
    queue.clear();
    return n;
}

} // namespace blescanner
