// Tapas de los discos: se buscan en la carpeta (cover/folder/front.jpg|png)
// o embebidas en el MP3 (APIC), se achican a SIZE x SIZE y se guardan en
// /music/.mp3player/art para no decodificarlas de nuevo.
#pragma once

#include <M5Cardputer.h>

namespace art {

static constexpr int SIZE = 88;

// Dibuja la tapa del disco en spr (SIZE x SIZE, 16 bits). false = sin tapa.
bool load(int album, LGFX_Sprite &spr);
void clearCache();

} // namespace art
