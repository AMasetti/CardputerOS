// Tabla de firmas conocidas: company ID de manufacturer data -> categoría.
//
// v1: tabla compilada en el binario (sin parseo de JSON en runtime, el repo
// no usa ArduinoJson en ningún lado y no queríamos sumar esa dependencia
// solo para esto). Está pensada para poder migrar a un archivo en SD más
// adelante sin tocar los callers: toda la lógica pasa por match(), que hoy
// mira el array estático y el día de mañana puede mirar algo cargado de SD.
//
// Fuentes de los company IDs (ver conversación / docs/BLE_SENTINEL_SPEC.md):
// proyectos NoPeek y "Nearby Glasses" reportan estos valores para smart
// glasses con cámara. Hay discrepancias menores entre ambos proyectos sobre
// algunos de ellos -> quedan marcados `verified = false` hasta confirmarlos
// contra una captura real o el registro oficial del Bluetooth SIG.
//
// El resto de la tabla (drones/robots, cámaras de acción, y el bucket
// genérico de "otro fabricante identificado") sí viene del registro
// oficial de company identifiers del Bluetooth SIG
// (https://www.bluetooth.com/specifications/assigned-numbers/, YAML público
// en bitbucket.org/bluetooth-SIG/public) -- esos quedan `verified = true`
// porque la fuente es la autoridad que asigna los IDs, no un proyecto de
// terceros reconstruyéndolos. Es un subconjunto curado a mano (los
// fabricantes de consumo más reconocibles), no las ~4000 entradas
// completas del registro -- la mayoría son integradores B2B/industriales
// sin relevancia para detectar vigilancia, y cargarlas todas infla el
// binario sin agregar nada útil a esta app.
#pragma once

#include <Arduino.h>

namespace signaturedb {

enum class Category : uint8_t {
    Unknown,        // sin match -> tratado como sospechoso por defecto
    SmartGlasses,   // Meta Ray-Ban/Oakley, Snap Spectacles -- también cámaras de
                    // acción (GoPro, Insta360): el bucket "meta o filmación" de
                    // la UI (azul) agrupa cualquier dispositivo cuyo propósito
                    // central es grabar, no solo anteojos.
    DroneRemoteId,  // ASTM F3411 Remote ID -- matcheado por `remoteIdService`
                    // (ver ble_scanner.h/fingerprint.h), NO por company ID:
                    // es el UUID 0xFFFA en el AD type Service Data (0x16),
                    // asignado por el Bluetooth SIG a "ASTM Remote ID"
                    // (registro oficial, assigned_numbers/uuids/sdo_uuids.yaml)
                    // -- confirmado contra la fuente, no de un proyecto de
                    // terceros. Solo detecta presencia del UUID, no decodifica
                    // el mensaje ODID (Basic ID/Location/Self-ID/etc.).
                    // También robots (iRobot, Anki, Ninebot/Segway, por
                    // company ID igual que el resto de IdentifiedVendor):
                    // bucket "drones y robots" de la UI (violeta). No son lo
                    // mismo (un Roomba no vuela), pero comparten el mismo
                    // nivel de atención ("hay algo autónomo/teleoperado
                    // cerca") y no justifican una categoría de UI separada.
    Headset,        // heurística por nombre, ver matchByName()
    FindMyTracker,  // accesorio Find My de Apple (ver matchFindMyNetwork()) -- AirTag es
                    // el caso más común, pero el mismo type byte lo usan otros
                    // accesorios Find My, así que no afirmamos "es un AirTag" puntual.
    IdentifiedVendor, // company ID matcheado contra el registro general de
                      // Bluetooth SIG (ver matchByCompanyId()/TABLE) pero sin
                      // ninguna razón particular para alertar -- un Garmin, un
                      // Sonos, un router. Bucket "otro dispositivo identificado"
                      // de la UI (verde == "todo tranquilo"): se diferencia de
                      // Unknown en que SÍ sabemos qué es, solo que no es un tipo
                      // de dispositivo que esta app esté pensada para vigilar.
};

// Nombres de perfil de alerta que consume alert_engine. Separado de
// Category porque a futuro una misma categoría podría tener distintos
// perfiles según confianza/contexto.
enum class AlertProfile : uint8_t {
    SuspiciousDefault,
    HeadsetLowPriority,
    Silent,
    ConvoySuspect, // disparado por convoy_analyzer, no por match de firma
};

struct Entry {
    uint16_t companyId;
    Category category;
    AlertProfile alertProfile;
    const char *vendorLabel;
    bool verified; // false = reportado por terceros, no confirmado por nosotros
};

// Busca por company ID de manufacturer data. Devuelve nullptr si no hay
// match -> el caller debe tratarlo como Category::Unknown / SuspiciousDefault
// (postura conservadora: lo no identificado es lo más prioritario, no lo
// menos, ver spec sección AlertEngine).
const Entry *matchByCompanyId(uint16_t companyId);

// Heurística v1 para auriculares: substring en el nombre del dispositivo
// (AD type 0x09, Complete/Shortened Local Name). No es tan confiable como
// un company ID -especialmente porque muchos fabricantes también usan esos
// mismos nombres genéricos para otra cosa- pero sirve como segundo perfil
// de alerta real para probar el AlertEngine mientras no tenemos una tabla
// de company IDs de auriculares verificada.
bool matchHeadsetByName(const String &localName);

// Find My de Apple (AirTag y otros accesorios): company ID 0x004C (Apple)
// + type byte 0x12 dentro del manufacturer data. Company ID solo no
// alcanza -- TODOS los dispositivos Apple (iPhone, AirPods, Watch) usan
// 0x004C para distintas cosas, el type byte es el que distingue el
// protocolo Find My específicamente. Fuente: reverse engineering público
// (Adam Catley, paper arXiv 2401.13584 sobre seguridad de AirTag/SmartTag),
// no documentación oficial de Apple -- ver docs/BLE_SENTINEL_SPEC.md.
//
// No distingue "cerca de su dueño" vs "separado/perdido" (ese bit del
// status byte varía entre fuentes consultadas y no se confirmó contra una
// captura propia) -- v1 solo dice "hay un accesorio Find My anunciando
// cerca", que alcanza para el caso de uso de detectar trackers ajenos.
bool matchFindMyNetwork(uint16_t companyId, uint8_t mfgTypeByte);

} // namespace signaturedb
