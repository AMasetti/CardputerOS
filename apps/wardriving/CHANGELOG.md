# Changelog — Wardriving pasivo

## 0.2.0
- Pantalla nueva: la anterior era ilegible en el hardware, con todo apretado en una línea. Ahora hay un
  dashboard con filas separadas, estados en texto y color con significado fijo (como BLE Sentinel):
  - el encabezado dice `CAPTURA ACTIVA` (rojo), `PAUSADA` (amarillo) o `DETENIDA` (gris), con el tiempo
    capturado en la sesión;
  - `Sesion XXXX` y `SD: GRABANDO` (verde), `RAM` (amarillo), `ERROR` (rojo) o `SIN SESION`;
  - BLE y LoRa: `ESCUCHANDO`, `APAGADO` o `ERROR`, con `ids` (identidades) y `ev` (eventos de la sesión);
    en pausa, las radios activadas dicen `listo`;
  - WiFi: `NO DISP` (`ABI 1.8`);
  - GPS: `FIX` (solo con un fix fresco), `BUSCANDO`, `SIN FIX`, `APAGADO` o `ERROR`;
  - la última actividad, las muestras, el lote pendiente y los controles, en dos líneas.
- Ayuda (`H` o `?`) con una acción por línea. `H` otra vez abre el diagnóstico: BLE pasivo, WiFi no
  disponible y su motivo, canal LoRa, GPS (puerto, NMEA, fix, satélites, edad), SD y su motivo,
  archivo de sesión y versión. `H` vuelve al dashboard. `esc` vuelve directo y `w` abre el diagnóstico.
- Los avisos de cada tecla se ven 2,5 s en lugar de "Ultimo:".
- Sin cambios en la captura, el formato de archivos ni la lógica pasiva.

## 0.1.0
- Primera versión. Solo escucha: no transmite, no anuncia, no conecta ni se asocia en ninguna radio.
- BLE: scan pasivo con identidad estable (fingerprint de BLE Sentinel; la MAC es un dato efímero).
  Guarda RSSI mín/máx/promedio, primera/última vez, nombre y categoría por identidad (hasta 48).
- LoRa: solo recepción en 915.000 MHz, SF9, BW125, sync 0x12, a la vista en pantalla. RSSI, SNR,
  frecuencia y payload en hex truncado, sin interpretar (hasta 32 identidades).
- WiFi: `unsupported`. La ABI 1.8 solo tiene un scan STA activo (manda probe requests) y sin BSSID. El
  README documenta la extensión de ABI necesaria. La app apaga el WiFi del OS mientras corre.
- GPS del Cap LoRa-1262 (`gnss.h` de Meshtastic): las muestras sin fix van marcadas con `gps_valid=false`.
- Sesiones en `/sdcard/wardriving/sessions`: CSV `.part` append-only por lotes (5 s o 32 filas) que
  pasa a `.csv` al cerrar, más GeoJSON y JSON de metadatos. Al arrancar, recupera los `.part` que
  quedaron sin cerrar. Nunca borra nada.
