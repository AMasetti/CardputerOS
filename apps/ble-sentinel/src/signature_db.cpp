#include "signature_db.h"

namespace signaturedb {

namespace {

const Entry TABLE[] = {
    // Luxottica (Ray-Ban Meta, Oakley Meta). Reportado por NoPeek y el
    // artículo de ZuckOff; NoPeek y "Nearby Glasses" coinciden en este valor.
    {0x0D53, Category::SmartGlasses, AlertProfile::SuspiciousDefault, "Luxottica (Ray-Ban/Oakley Meta)", true},

    // Meta Platforms Technologies (Quest y otros wearables Meta).
    {0x058E, Category::SmartGlasses, AlertProfile::SuspiciousDefault, "Meta Platforms Technologies", true},

    // Reportado por el proyecto "Nearby Glasses" como Meta Platforms, pero
    // un comentario de usuarios de NoPeek lo pone en duda -> sin verificar.
    {0x01AB, Category::SmartGlasses, AlertProfile::SuspiciousDefault, "Meta Platforms (sin confirmar)", false},

    // Snap Inc. (Spectacles), reportado por "Nearby Glasses".
    {0x03C2, Category::SmartGlasses, AlertProfile::SuspiciousDefault, "Snap Inc. (Spectacles)", false},

    // --- Cámaras de acción -- mismo bucket que smart glasses ("meta o
    // filmación", ver signature_db.h): el punto en común es "esto está
    // diseñado para grabar", no la forma del dispositivo. Fuente: registro
    // oficial de company identifiers del Bluetooth SIG.
    {0x02F2, Category::SmartGlasses, AlertProfile::SuspiciousDefault, "GoPro, Inc.", true},
    {0x10D7, Category::SmartGlasses, AlertProfile::SuspiciousDefault, "Insta360 (Arashi Vision Inc.)", true},

    // --- Drones y robots (bucket violeta, ver signature_db.h). DJI es el
    // fabricante de drones de consumo más común; iRobot/Anki/Ninebot son
    // robots domésticos (aspiradoras, juguetes robóticos, patinetas
    // autobalanceadas) -- no vuelan, pero son el mismo tipo de atención
    // ("hay algo autónomo cerca"). Fuente: registro oficial del Bluetooth SIG.
    {0x08AA, Category::DroneRemoteId, AlertProfile::SuspiciousDefault, "SZ DJI Technology Co., Ltd.", true},
    {0x0600, Category::DroneRemoteId, AlertProfile::SuspiciousDefault, "iRobot Corporation", true},
    {0x05F8, Category::DroneRemoteId, AlertProfile::SuspiciousDefault, "Anki Inc.", true},
    {0x0F1F, Category::DroneRemoteId, AlertProfile::SuspiciousDefault, "Ninebot (Segway)", true},

    // --- Otro dispositivo identificado (bucket verde == "todo tranquilo",
    // ver signature_db.h): subconjunto curado a mano del registro oficial
    // del Bluetooth SIG con los fabricantes de electrónica de consumo más
    // reconocibles -- wearables, audio, smart home, cómputo. AlertProfile
    // Silent a propósito: son la categoría "sabemos qué es y no es un tipo
    // de dispositivo que esta app vigile", no tiene sentido que suenen
    // igual que un sospechoso real sin identificar.
    {0x0075, Category::IdentifiedVendor, AlertProfile::Silent, "Samsung Electronics", true},
    {0x018E, Category::IdentifiedVendor, AlertProfile::Silent, "Google LLC", true},
    {0x0006, Category::IdentifiedVendor, AlertProfile::Silent, "Microsoft", true},
    {0x038F, Category::IdentifiedVendor, AlertProfile::Silent, "Xiaomi Inc.", true},
    {0x0087, Category::IdentifiedVendor, AlertProfile::Silent, "Garmin International", true},
    {0x00D1, Category::IdentifiedVendor, AlertProfile::Silent, "Polar Electro", true},
    {0x009F, Category::IdentifiedVendor, AlertProfile::Silent, "Suunto Oy", true},
    {0x0178, Category::IdentifiedVendor, AlertProfile::Silent, "Casio Computer Co.", true},
    {0x0040, Category::IdentifiedVendor, AlertProfile::Silent, "Seiko Epson Corporation", true},
    {0x0B8A, Category::IdentifiedVendor, AlertProfile::Silent, "Seiko Instruments", true},
    {0x012D, Category::IdentifiedVendor, AlertProfile::Silent, "Sony Corporation", true},
    {0x009E, Category::IdentifiedVendor, AlertProfile::Silent, "Bose Corporation", true},
    {0x0057, Category::IdentifiedVendor, AlertProfile::Silent, "Harman International (JBL)", true},
    {0x00CC, Category::IdentifiedVendor, AlertProfile::Silent, "Beats Electronics", true},
    {0x01DA, Category::IdentifiedVendor, AlertProfile::Silent, "Logitech International", true},
    {0x0059, Category::IdentifiedVendor, AlertProfile::Silent, "Nordic Semiconductor", true},
    {0x000D, Category::IdentifiedVendor, AlertProfile::Silent, "Texas Instruments", true},
    {0x02E5, Category::IdentifiedVendor, AlertProfile::Silent, "Espressif Systems", true},
    {0x00D2, Category::IdentifiedVendor, AlertProfile::Silent, "Renesas", true},
    {0x0131, Category::IdentifiedVendor, AlertProfile::Silent, "Cypress Semiconductor", true},
    {0x000F, Category::IdentifiedVendor, AlertProfile::Silent, "Broadcom", true},
    {0x005D, Category::IdentifiedVendor, AlertProfile::Silent, "Realtek Semiconductor", true},
    {0x067C, Category::IdentifiedVendor, AlertProfile::Silent, "Tile, Inc.", true},
    {0x08C3, Category::IdentifiedVendor, AlertProfile::Silent, "Chipolo d.o.o.", true},
    {0x0CC2, Category::IdentifiedVendor, AlertProfile::Silent, "Anker Innovations", true},
    {0x0055, Category::IdentifiedVendor, AlertProfile::Silent, "Plantronics / Poly", true},
    {0x07C9, Category::IdentifiedVendor, AlertProfile::Silent, "Skullcandy", true},
    {0x03FF, Category::IdentifiedVendor, AlertProfile::Silent, "Withings", true},
    {0x0304, Category::IdentifiedVendor, AlertProfile::Silent, "Oura Health", true},
    {0x0171, Category::IdentifiedVendor, AlertProfile::Silent, "Amazon.com Services", true},
    {0x01B5, Category::IdentifiedVendor, AlertProfile::Silent, "Nest Labs (Google)", true},
    {0x027D, Category::IdentifiedVendor, AlertProfile::Silent, "Huawei Technologies", true},
    {0x072F, Category::IdentifiedVendor, AlertProfile::Silent, "OnePlus", true},
    {0x079A, Category::IdentifiedVendor, AlertProfile::Silent, "Oppo Mobile (incl. Realme)", true},
    {0x0CCB, Category::IdentifiedVendor, AlertProfile::Silent, "Nothing Technology", true},
    {0x05A7, Category::IdentifiedVendor, AlertProfile::Silent, "Sonos Inc", true},
    {0x065A, Category::IdentifiedVendor, AlertProfile::Silent, "Marshall Group", true},
    {0x04AD, Category::IdentifiedVendor, AlertProfile::Silent, "Shure Inc", true},
    {0x0494, Category::IdentifiedVendor, AlertProfile::Silent, "Sennheiser electronic", true},
    {0x00C4, Category::IdentifiedVendor, AlertProfile::Silent, "LG Electronics", true},
    {0x01A9, Category::IdentifiedVendor, AlertProfile::Silent, "Canon Inc.", true},
    {0x0399, Category::IdentifiedVendor, AlertProfile::Silent, "Nikon Corporation", true},
    {0x04D8, Category::IdentifiedVendor, AlertProfile::Silent, "FUJIFILM Corporation", true},
    {0x01F1, Category::IdentifiedVendor, AlertProfile::Silent, "Zebra Technologies", true},
    {0x0526, Category::IdentifiedVendor, AlertProfile::Silent, "Honeywell International", true},
    {0x022B, Category::IdentifiedVendor, AlertProfile::Silent, "Tesla, Inc.", true},
    {0x0103, Category::IdentifiedVendor, AlertProfile::Silent, "Bang & Olufsen", true},
    {0x02ED, Category::IdentifiedVendor, AlertProfile::Silent, "HTC Corporation", true},
    {0x04EC, Category::IdentifiedVendor, AlertProfile::Silent, "Motorola Solutions", true},
    {0x02C5, Category::IdentifiedVendor, AlertProfile::Silent, "Lenovo", true},
    {0x10D1, Category::IdentifiedVendor, AlertProfile::Silent, "Acer Inc.", true},
    {0x0E41, Category::IdentifiedVendor, AlertProfile::Silent, "Asustek Computer", true},
    {0x041E, Category::IdentifiedVendor, AlertProfile::Silent, "Dell Computer", true},
    {0x0065, Category::IdentifiedVendor, AlertProfile::Silent, "HP, Inc.", true},
    {0x0553, Category::IdentifiedVendor, AlertProfile::Silent, "Nintendo Co.", true},
    {0x005C, Category::IdentifiedVendor, AlertProfile::Silent, "Belkin International", true},
    {0x01DD, Category::IdentifiedVendor, AlertProfile::Silent, "Philips (Signify)", true},
    {0x0870, Category::IdentifiedVendor, AlertProfile::Silent, "Wyze Labs", true},
    {0x0C19, Category::IdentifiedVendor, AlertProfile::Silent, "Arlo Technologies", true},
    {0x0446, Category::IdentifiedVendor, AlertProfile::Silent, "NETGEAR, Inc.", true},
    {0x083F, Category::IdentifiedVendor, AlertProfile::Silent, "D-Link Corp.", true},
    {0x021B, Category::IdentifiedVendor, AlertProfile::Silent, "Cisco Systems", true},
};

constexpr size_t TABLE_LEN = sizeof(TABLE) / sizeof(TABLE[0]);

// Substrings típicos de nombre de dispositivo para auriculares/headsets.
// Heurística declarada en signature_db.h: no reemplaza una tabla de company
// ID real, es un placeholder para tener un segundo AlertProfile funcionando.
const char *const HEADSET_NAME_HINTS[] = {
    "airpods", "buds", "headset", "headphone", "earphone", "wf-", "wh-",
};
constexpr size_t HEADSET_HINTS_LEN = sizeof(HEADSET_NAME_HINTS) / sizeof(HEADSET_NAME_HINTS[0]);

constexpr uint16_t APPLE_COMPANY_ID = 0x004C;
constexpr uint8_t FINDMY_TYPE_BYTE = 0x12;

} // namespace

const Entry *matchByCompanyId(uint16_t companyId)
{
    for (size_t i = 0; i < TABLE_LEN; i++) {
        if (TABLE[i].companyId == companyId) return &TABLE[i];
    }
    return nullptr;
}

bool matchHeadsetByName(const String &localName)
{
    if (localName.isEmpty()) return false;
    String lower = localName;
    lower.toLowerCase();
    for (size_t i = 0; i < HEADSET_HINTS_LEN; i++) {
        if (lower.indexOf(HEADSET_NAME_HINTS[i]) >= 0) return true;
    }
    return false;
}

bool matchFindMyNetwork(uint16_t companyId, uint8_t mfgTypeByte)
{
    return companyId == APPLE_COMPANY_ID && mfgTypeByte == FINDMY_TYPE_BYTE;
}

} // namespace signaturedb
