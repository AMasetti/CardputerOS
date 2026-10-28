// Exportación del sighting_store actual como CSV + JSON, con un hash
// SHA-256 al lado de cada archivo para detectar si se tocó después.
//
// Esto es integridad básica, NO es firma criptográfica con clave privada
// ni timestamp de autoridad certificada -- si el uso real es documentar
// algo serio (ej. una denuncia), el registro ayuda pero no sustituye
// asesoría legal sobre qué es admisible. Ver docs/BLE_SENTINEL_SPEC.md
// sección 6.
#pragma once

#include <Arduino.h>

namespace exportmodule {

void begin(bool sdReady); // crea /sentinel/exports si hay SD

// Exporta todo el sighting_store actual. Devuelve el nombre base usado
// (sin extensión, ej. "export_s3_002") o cadena vacía si no hay SD o
// falló la escritura.
String exportNow();

} // namespace exportmodule
