// Node settings shared by the two Meshtastic apps (with screen, and Bluetooth-only): the same identity,
// names, radio and channel whichever runs. Each app keeps a copy in its own preferences; the file on the
// SD card (/sdcard/meshtastic/settings.bin) is the common one and wins when present.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "mesh_core.h"

struct Settings {
    char long_name[40];
    char short_name[5];
    uint8_t region;       // REGIONS[]
    uint8_t preset;       // PRESETS[]
    char channel[12];     // "" = el nombre del preset (canal público)
    uint8_t psk[32];
    uint8_t psk_len;
    uint16_t slot;        // 0 = por nombre de canal
    int8_t power;         // dBm
    uint8_t hops;
    bool relay, gps, share_pos, sound;
};

inline void settings_default(Settings &S, uint32_t node) {
    memset(&S, 0, sizeof(S));
    snprintf(S.long_name, sizeof(S.long_name), "Cardputer %04x", (unsigned)(node & 0xFFFF));
    snprintf(S.short_name, sizeof(S.short_name), "%04x", (unsigned)(node & 0xFFFF));
    S.region = 0;   // ANZ: Argentina
    S.preset = 0;   // LongFast
    S.psk[0] = 1;   // clave pública por defecto ("AQ==")
    S.psk_len = 1;
    S.power = 22;
    S.hops = 3;
    S.relay = true;
    S.gps = true;
    S.share_pos = false; // la posición se comparte solo si el usuario lo pide
    S.sound = true;
}

inline void settings_sanitize(Settings &S) {
    if (S.region >= mesh::NUM_REGIONS) S.region = 0;
    if (S.preset >= mesh::NUM_PRESETS) S.preset = 0;
    if (S.hops < 1 || S.hops > 7) S.hops = 3;
    if (S.power < 2 || S.power > 22) S.power = 22;
    S.long_name[sizeof(S.long_name) - 1] = 0;
    S.short_name[sizeof(S.short_name) - 1] = 0;
    S.channel[sizeof(S.channel) - 1] = 0;
    if (S.psk_len > 32) S.psk_len = 1;
}

constexpr const char *SHARED_SETTINGS = "/sdcard/meshtastic/settings.bin";
constexpr uint32_t SHARED_MAGIC = 0x4D534853; // "SHSM"

struct SharedSettings {
    uint32_t magic, size, node;
    uint8_t ok_mqtt, pad[3];
    Settings s;
};

inline bool settings_load_shared(uint32_t &node, Settings &s, bool &ok_mqtt) {
    FILE *f = fopen(SHARED_SETTINGS, "rb");
    if (!f) return false;
    SharedSettings x;
    bool ok = fread(&x, sizeof(x), 1, f) == 1 && x.magic == SHARED_MAGIC && x.size == sizeof(Settings) && x.node >= 0x10 &&
              x.node != mesh::BROADCAST;
    fclose(f);
    if (!ok) return false;
    node = x.node;
    s = x.s;
    ok_mqtt = x.ok_mqtt;
    settings_sanitize(s);
    return true;
}

inline void settings_save_shared(uint32_t node, const Settings &s, bool ok_mqtt) {
    SharedSettings x = {};
    x.magic = SHARED_MAGIC;
    x.size = sizeof(Settings);
    x.node = node;
    x.ok_mqtt = ok_mqtt;
    x.s = s;
    mkdir("/sdcard/meshtastic", 0777);
    FILE *f = fopen(SHARED_SETTINGS, "wb");
    if (!f) return;
    fwrite(&x, sizeof(x), 1, f);
    fclose(f);
}
