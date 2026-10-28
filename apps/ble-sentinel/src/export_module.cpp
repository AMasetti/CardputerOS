#include "export_module.h"
#include <SD.h>
#include <mbedtls/md.h>
#include "sighting_store.h"
#include "convoy_analyzer.h"

namespace exportmodule {

namespace {

constexpr const char *EXPORTS_DIR = "/sentinel/exports";
constexpr const char *EXPORT_LOG = "/sentinel/export_log.txt";

bool g_sdOk = false;
uint16_t g_exportCounter = 0;

const char *categoryName(signaturedb::Category c)
{
    switch (c) {
        case signaturedb::Category::SmartGlasses: return "smart_glasses";
        case signaturedb::Category::DroneRemoteId: return "drone_remote_id";
        case signaturedb::Category::Headset: return "headset";
        case signaturedb::Category::FindMyTracker: return "findmy_tracker";
        case signaturedb::Category::IdentifiedVendor: return "identified_vendor";
        default: return "unknown";
    }
}

void toHex(const uint8_t *data, size_t len, char *out /* cabe len*2+1 */)
{
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = DIGITS[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = DIGITS[data[i] & 0xF];
    }
    out[len * 2] = '\0';
}

// SHA-256 incremental vía la API genérica de mbedtls (mbedtls_md_*): es la
// más estable entre versiones del toolchain -- la API sha256-específica
// (mbedtls_sha256_starts/_ret) cambió de nombre entre releases y no
// podemos validar contra el bundle exacto de este framework sin compilar
// en el hardware real. Si el CI la rechaza, es la primera sospechosa.
bool sha256File(const char *path, char hexOut[65])
{
    File f = SD.open(path);
    if (!f) return false;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md_setup(&ctx, info, 0) != 0) {
        mbedtls_md_free(&ctx);
        f.close();
        return false;
    }
    mbedtls_md_starts(&ctx);

    uint8_t buf[256];
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
        mbedtls_md_update(&ctx, buf, n);
    }

    uint8_t digest[32];
    mbedtls_md_finish(&ctx, digest);
    mbedtls_md_free(&ctx);
    f.close();

    toHex(digest, sizeof(digest), hexOut);
    return true;
}

void writeSidecarAndLog(const char *path, uint32_t nowMs)
{
    char hex[65];
    if (!sha256File(path, hex)) return;

    String sidecarPath = String(path) + ".sha256";
    if (File sc = SD.open(sidecarPath.c_str(), FILE_WRITE)) {
        sc.print(hex);
        sc.close();
    }

    if (File log = SD.open(EXPORT_LOG, FILE_WRITE)) {
        log.printf("{\"ts_rel_ms\":%lu,\"file\":\"%s\",\"sha256\":\"%s\"}\n",
                   static_cast<unsigned long>(nowMs), path, hex);
        log.close();
    }
}

void writeCsv(const char *path)
{
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    f.println("fingerprint_hash,last_mac,category,confidence_low,trusted,rssi,times_seen,"
               "mac_changes,convoy_score,first_seen_rel_ms,last_seen_rel_ms");
    size_t n = sightingstore::count();
    for (size_t i = 0; i < n; i++) {
        const sightingstore::Record *r = sightingstore::byIndex(i);
        if (!r) continue;
        f.printf("%016llx,%s,%s,%s,%s,%d,%u,%u,%u,%lu,%lu\n",
                  static_cast<unsigned long long>(r->fingerprintHash),
                  r->lastMac.c_str(), categoryName(r->category),
                  r->confidenceLow ? "true" : "false",
                  r->trusted ? "true" : "false", r->lastRssi,
                  r->timesSeen, r->macChanges,
                  convoyanalyzer::score(r->fingerprintHash),
                  static_cast<unsigned long>(r->firstSeenMs),
                  static_cast<unsigned long>(r->lastSeenMs));
    }
    f.close();
}

void writeJson(const char *path, uint32_t sessionId)
{
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    // timestamp_type explícito: ver export_module.h / sighting_store.h --
    // nada de esto es hora de pared mientras no haya RTC.
    f.printf("{\"timestamp_type\":\"relative_since_boot\",\"session_id\":%lu,"
              "\"app\":\"ble-sentinel\",\"sightings\":[\n",
              static_cast<unsigned long>(sessionId));
    size_t n = sightingstore::count();
    for (size_t i = 0; i < n; i++) {
        const sightingstore::Record *r = sightingstore::byIndex(i);
        if (!r) continue;
        f.printf("  {\"fingerprint_hash\":\"%016llx\",\"last_mac\":\"%s\",\"category\":\"%s\","
                  "\"confidence_low\":%s,\"trusted\":%s,\"rssi\":%d,\"times_seen\":%u,\"mac_changes\":%u,"
                  "\"convoy_score\":%u,\"first_seen_rel_ms\":%lu,\"last_seen_rel_ms\":%lu}%s\n",
                  static_cast<unsigned long long>(r->fingerprintHash),
                  r->lastMac.c_str(), categoryName(r->category),
                  r->confidenceLow ? "true" : "false",
                  r->trusted ? "true" : "false", r->lastRssi,
                  r->timesSeen, r->macChanges,
                  convoyanalyzer::score(r->fingerprintHash),
                  static_cast<unsigned long>(r->firstSeenMs),
                  static_cast<unsigned long>(r->lastSeenMs),
                  (i + 1 < n) ? "," : "");
    }
    f.println("]}");
    f.close();
}

} // namespace

void begin(bool sdReady)
{
    g_sdOk = sdReady;
    if (g_sdOk && !SD.exists(EXPORTS_DIR)) SD.mkdir(EXPORTS_DIR);
}

String exportNow()
{
    if (!g_sdOk) return "";

    g_exportCounter++;
    char base[48];
    snprintf(base, sizeof(base), "%s/export_s%lu_%03u", EXPORTS_DIR,
              static_cast<unsigned long>(sightingstore::sessionId()), g_exportCounter);

    String csvPath = String(base) + ".csv";
    String jsonPath = String(base) + ".json";

    writeCsv(csvPath.c_str());
    writeJson(jsonPath.c_str(), sightingstore::sessionId());

    uint32_t now = millis();
    writeSidecarAndLog(csvPath.c_str(), now);
    writeSidecarAndLog(jsonPath.c_str(), now);

    // Solo el nombre de archivo (sin el directorio) para mostrar en la UI.
    String baseStr(base);
    int slash = baseStr.lastIndexOf('/');
    return slash >= 0 ? baseStr.substring(slash + 1) : baseStr;
}

} // namespace exportmodule
