# Changelog — Wardriving pasivo

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
