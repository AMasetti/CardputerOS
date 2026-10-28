#include "sighting_store.h"
#include <SD.h>
#include <cstdlib>
#include "fingerprint.h"

namespace sightingstore {

namespace {

constexpr const char *DATA_DIR = "/sentinel";
constexpr const char *HISTORY_DIR = "/sentinel/history";
constexpr const char *SESSION_FILE = "/sentinel/session.txt";
constexpr const char *TRUSTED_FILE = "/sentinel/trusted.txt";

std::vector<Record> records;
std::vector<uint64_t> trustedHashes; // cargado de TRUSTED_FILE en begin()
uint32_t g_sessionId = 0;
bool g_sdAvailable = false;
bool g_loggingEnabled = false; // apagado por defecto -- ver setLoggingEnabled() en el header
File historyLog;

const char *categoryLabel(signaturedb::Category c)
{
    switch (c) {
        case signaturedb::Category::SmartGlasses: return "glasses";
        case signaturedb::Category::DroneRemoteId: return "drone";
        case signaturedb::Category::Headset: return "headset";
        case signaturedb::Category::FindMyTracker: return "findmy";
        case signaturedb::Category::IdentifiedVendor: return "identified_vendor";
        default: return "unknown";
    }
}

bool hashInTrustedList(uint64_t hash)
{
    for (uint64_t h : trustedHashes) {
        if (h == hash) return true;
    }
    return false;
}

// Reescribe el archivo entero desde trustedHashes en vez de confiar en el
// modo de apertura para truncar/agregar -- es una lista chica (unos pocos
// dispositivos propios como mucho), reescribirla completa es barato y sin
// ambigüedad.
void saveTrustedFile()
{
    if (!g_sdAvailable) return;
    SD.remove(TRUSTED_FILE);
    if (trustedHashes.empty()) return;
    File f = SD.open(TRUSTED_FILE, FILE_WRITE);
    if (!f) return;
    for (uint64_t h : trustedHashes) {
        f.printf("%016llx\n", static_cast<unsigned long long>(h));
    }
    f.close();
}

void loadTrustedFile()
{
    trustedHashes.clear();
    if (!g_sdAvailable || !SD.exists(TRUSTED_FILE)) return;
    File f = SD.open(TRUSTED_FILE);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        uint64_t h = strtoull(line.c_str(), nullptr, 16);
        if (h != 0) trustedHashes.push_back(h);
    }
    f.close();
}

// session.txt guarda un único entero: el próximo session_id a usar. Si no
// existe (primera vez, o no hay SD) arrancamos en 1.
uint32_t loadAndBumpSessionId()
{
    uint32_t id = 1;
    if (SD.exists(SESSION_FILE)) {
        File f = SD.open(SESSION_FILE);
        if (f) {
            id = f.parseInt();
            f.close();
            if (id == 0) id = 1;
        }
    }
    File f = SD.open(SESSION_FILE, FILE_WRITE);
    if (f) {
        f.print(id + 1);
        f.close();
    }
    return id;
}

void openHistoryLog()
{
    char path[48];
    snprintf(path, sizeof(path), "%s/session_%lu.log", HISTORY_DIR,
              static_cast<unsigned long>(g_sessionId));
    historyLog = SD.open(path, FILE_WRITE);
}

// Una línea JSON simple por evento loggeable. No parseamos JSON en el
// dispositivo (ver signature_db.h) pero escribirlo en ese formato deja el
// archivo listo para herramientas externas (export, análisis) sin tener que
// definir un formato propio.
void appendLogLine(const Record &r, SightingKind kind)
{
    if (!g_loggingEnabled || !historyLog) return;
    const char *kindStr = kind == SightingKind::New ? "new"
                          : kind == SightingKind::MacRotated ? "mac_rotated"
                                                              : "update";
    historyLog.printf(
        "{\"session_rel_ms\":%lu,\"fp\":\"%016llx\",\"mac\":\"%s\",\"rssi\":%d,"
        "\"category\":\"%s\",\"kind\":\"%s\",\"confidence_low\":%s}\n",
        static_cast<unsigned long>(r.lastSeenMs),
        static_cast<unsigned long long>(r.fingerprintHash),
        r.lastMac.c_str(), r.lastRssi, categoryLabel(r.category), kindStr,
        r.confidenceLow ? "true" : "false");
    historyLog.flush();
}

Record *find(uint64_t hash)
{
    for (auto &r : records) {
        if (r.fingerprintHash == hash) return &r;
    }
    return nullptr;
}

} // namespace

void begin(bool sdReady)
{
    g_sdAvailable = sdReady;
    if (g_sdAvailable) {
        if (!SD.exists(DATA_DIR)) SD.mkdir(DATA_DIR);
        if (!SD.exists(HISTORY_DIR)) SD.mkdir(HISTORY_DIR);
        g_sessionId = loadAndBumpSessionId();
        openHistoryLog();
    } else {
        g_sessionId = 1; // degradado: solo RAM, sin continuidad entre boots
    }
    loadTrustedFile(); // sí sobrevive entre sesiones, a diferencia de `records`
    records.clear();
    records.reserve(32);
}

uint32_t sessionId() { return g_sessionId; }
bool sdAvailable() { return g_sdAvailable; }

void resetSession()
{
    records.clear();
    // `trustedHashes` NO se toca -- lo marcado como propio sigue siéndolo
    // entre reinicios de sesión, igual que entre reinicios reales del
    // dispositivo (persiste a SD independiente de esto).
    if (g_sdAvailable) {
        if (historyLog) historyLog.close(); // si no, el archivo viejo queda abierto y pisado
        g_sessionId = loadAndBumpSessionId();
        openHistoryLog();
    } else {
        g_sessionId++; // sin SD no hay persistencia real, es solo para que el HUD muestre otro número
    }
}

void setLoggingEnabled(bool on) { g_loggingEnabled = on; }
bool loggingEnabled() { return g_loggingEnabled; }

SightingKind record(const SightingInput &in, Record &out)
{
    uint32_t now = millis();
    Record *existing = find(in.fingerprintHash);

    if (!existing) {
        Record r;
        r.fingerprintHash = in.fingerprintHash;
        r.lastMac = in.mac;
        r.lastRssi = in.rssi;
        r.firstSeenMs = now;
        r.lastSeenMs = now;
        r.timesSeen = 1;
        r.macChanges = 0;
        r.category = in.category;
        r.confidenceLow = in.confidenceLow;
        r.lastCompanyId = in.companyId;
        r.lastMfgTypeByte = in.mfgTypeByte;
        r.lastTxPowerDbm = in.txPowerDbm;
        r.lastPayloadLen = in.payloadLen;
        r.lastLocalName = in.localName;
        r.trusted = hashInTrustedList(in.fingerprintHash);
        records.push_back(r);
        out = r;
        appendLogLine(out, SightingKind::New);
        return SightingKind::New;
    }

    bool macChanged = (existing->lastMac != in.mac);
    SightingKind kind = SightingKind::Update;
    if (macChanged) {
        uint32_t delta = now - existing->lastSeenMs;
        // Solo lo tratamos como rotación "plausible" (y lo loggeamos como tal)
        // si el intervalo es compatible; si no, es simplemente ruido de
        // RSSI/timing y lo anotamos igual pero sin la etiqueta especial.
        kind = fingerprint::isPlausibleMacRotation(delta) ? SightingKind::MacRotated
                                                           : SightingKind::Update;
        existing->macChanges++;
    }

    existing->lastMac = in.mac;
    existing->lastRssi = in.rssi;
    existing->lastSeenMs = now;
    existing->timesSeen++;
    // El nombre local a veces llega recién en el scan response, no en el
    // primer paquete -- si este avistamiento lo trae, lo actualizamos;
    // si no, dejamos el que ya teníamos en vez de borrarlo con uno vacío.
    if (in.localName.length()) existing->lastLocalName = in.localName;
    existing->lastCompanyId = in.companyId;
    existing->lastMfgTypeByte = in.mfgTypeByte;
    existing->lastTxPowerDbm = in.txPowerDbm;
    existing->lastPayloadLen = in.payloadLen;
    // category/confidenceLow SÍ se reemplazan siempre, no solo al crear:
    // el caller (main.cpp) ya resolvió esto con los datos acumulados del
    // dispositivo, no solo el paquete de hoy, así que esta categoría es al
    // menos tan buena como la que ya teníamos -- nunca retrocede a Unknown
    // por un paquete que no trajo nombre, pero sí mejora en cuanto llega
    // una identificación nueva.
    existing->category = in.category;
    existing->confidenceLow = in.confidenceLow;
    out = *existing;

    // No logueamos cada Update (saturaría la SD con el ritmo de advertising
    // típico, cada ~100-1000ms) -- solo eventos que importan para el
    // historial: nuevo dispositivo o rotación de MAC detectada.
    if (kind != SightingKind::Update) appendLogLine(out, kind);

    return kind;
}

size_t count() { return records.size(); }

const Record *byIndex(size_t i)
{
    if (i >= records.size()) return nullptr;
    return &records[i];
}

const Record *byFingerprint(uint64_t fingerprintHash)
{
    return find(fingerprintHash);
}

void setTrusted(uint64_t fingerprintHash, bool trusted)
{
    bool already = hashInTrustedList(fingerprintHash);
    if (trusted && !already) {
        trustedHashes.push_back(fingerprintHash);
    } else if (!trusted && already) {
        for (size_t i = 0; i < trustedHashes.size(); i++) {
            if (trustedHashes[i] == fingerprintHash) {
                trustedHashes.erase(trustedHashes.begin() + i);
                break;
            }
        }
    }
    saveTrustedFile();

    // Si el dispositivo ya está en esta sesión, reflejarlo ahora mismo --
    // si no, el usuario vería el toggle sin efecto hasta el próximo
    // avistamiento.
    if (Record *existing = find(fingerprintHash)) {
        existing->trusted = trusted;
    }
}

bool isTrusted(uint64_t fingerprintHash)
{
    return hashInTrustedList(fingerprintHash);
}

} // namespace sightingstore
