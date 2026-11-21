# Wardriving pasivo

Relevamiento de señales BLE y LoRa con la posición del GPS del Cap LoRa-1262, para mirar después en un
GIS (QGIS, kepler.gl, uMap...). La pantalla no dibuja mapa: muestra estado y contadores.

## Solo escucha

La app **nunca** transmite, anuncia, conecta, se asocia, se autentica, desautentica ni intenta descifrar
nada, en ninguna de las tres radios:

| Radio | Qué hace | Qué no hace |
|---|---|---|
| BLE | observer con scan **pasivo** (`ble.scan_start(0, 0, …)`: `active = 0`, no manda SCAN_REQ) | no conecta, no lee GATT, no usa el servidor GATT (`ble_srv`), así que el OS no anuncia nada |
| WiFi | **`unsupported`** con la ABI actual (ver abajo). La app apaga el WiFi del OS (`net.mode(0)`) mientras corre para que el equipo no quede asociado a la red guardada | no escanea, no se asocia, no abre sockets |
| LoRa | solo recepción en un canal fijo | nunca llama `lora.send`; los paquetes ajenos se guardan como hex truncado, sin interpretarlos ni retransmitirlos |
| GNSS | lee NMEA (`apps/meshtastic/src/gnss.h`) | no escribe al receptor (`gnss_write`) |

`test/core_test.cpp` revisa `src/main.cpp` (sin comentarios ni strings) y falla si aparece alguna de esas
llamadas (`lora.send`, `ble.connect`, `ble_srv`, `net.connect`, `net.scan_start`, sockets, `gnss_write`…).

Al salir, el launcher del OS vuelve a conectar el WiFi guardado como con cualquier app: eso es del OS, no
de esta app.

### WiFi: por qué `unsupported`

La ABI 1.8 (`sdk/include/cpos/abi.h`) solo trae `net.scan_start`, el scan de estación del driver. En el OS
(`os/components/cpos_core/net.c`) usa `wifi_scan_config_t` por defecto, que es un scan **activo**: manda
probe requests en cada canal. Además `cpos_ap_t` no tiene BSSID, así que no hay identidad estable por AP.
Usarlo sería transmitir y llamarlo "escucha", así que no se usa ni se simula: la tecla `w` solo muestra el
diagnóstico, y la columna `source=wifi` existe en el formato pero esta versión nunca la escribe.

Extensión mínima de ABI para habilitarlo. Es una propuesta: no existe y no se inventó ningún símbolo:

1. **Mínima (beacons de AP):** agregar al final de `cpos_net_api_t` (minor 9)
   `int (*scan_passive)(int async, uint16_t dwell_ms);`, que hace el mismo scan con
   `scan_type = WIFI_SCAN_TYPE_PASSIVE` (sin probe requests), y
   `int (*scan_get_bssid)(int i, uint8_t bssid[6]);`.
2. **Completa (beacons + probe requests):** modo monitor de solo recepción con filtro de management frames:
   `int (*monitor_start)(uint16_t channel_mask, uint16_t dwell_ms); void (*monitor_stop)(void);`
   `int (*monitor_read)(cpos_wifi_mgmt_t *out);`, donde `cpos_wifi_mgmt_t` lleva
   `ms, rssi, channel, subtype, bssid[6], src[6], ssid_len, ssid[33]`. El OS lo arma con
   `esp_wifi_set_promiscuous` y `WIFI_PROMIS_FILTER_MASK_MGMT`, con la estación desconectada.
   Hay que verificar en el OS el consumo de RAM y el comportamiento con el driver en modo NULL.

Con cualquiera de las dos, el backend de esta app llenaría una tabla de 32 identidades (`WD_MAX_WIFI`)
con `identity = wifi:<bssid>`.

## LoRa: canal fijo

El SX1262 escucha **un canal por vez** y solo ve paquetes con los mismos parámetros. No se hace barrido
de bandas. El canal está en `LORA_CFG` (`src/main.cpp`) y se ve en pantalla y en el JSON:

- 915.000 MHz (ISM Argentina), BW 125 kHz, SF9, CR 4/5, sync word 0x12 (privado), preámbulo 8, CRC on.
- `power_dbm` queda en el mínimo (-9) aunque nunca se transmita.

Para otra red (por ejemplo, un canal Meshtastic: sync 0x2B y su frecuencia, BW y SF), hay que cambiar
`LORA_CFG` y recompilar. Si el hat no responde, la línea LoRa muestra `error` y el motivo (`lora.begin`
-2 = no responde el SX1262).

La identidad LoRa (`lora:<hash>`) agrupa por frecuencia y los primeros 8 bytes crudos del paquete,
tomados como bytes opacos: no se decodifica ningún protocolo.

## BLE: identidad

Es la misma que el fingerprint de BLE Sentinel: FNV-1a 64 sobre company ID, type byte del manufacturer
data, largo del payload, TX power en buckets de 4 dB, Remote ID (service data 0xFFFA) y UUIDs de 16 bits
ordenados. La MAC no entra en el hash: se guarda como dato efímero (`mac_or_addr`, la última vista). El
test compila `apps/ble-sentinel/src/fingerprint.cpp` y compara los hashes.

`fingerprint.cpp` no se incluye en la app porque usa `Arduino.h` y `std::vector`. La cuenta está reescrita
en `src/wd_core.h`, y el test garantiza que coincide. Diferencias deliberadas:

- Payload pobre (sin manufacturer data, UUIDs ni Remote ID): el hash de Sentinel no distingue
  dispositivos. Se mezcla la MAC y la identidad sale `ble:w:<hash>`, con categoría `unknown-weak`, para
  que el GIS sepa que esa identidad **no** sobrevive a una rotación de MAC.
- Las categorías son las de una tabla chica, no la `signature_db` de Sentinel (usa `Arduino.h`):
  `drone-rid`, `findmy`, `mfg:apple|microsoft|samsung|google|garmin|nordic|meta|espressif|sony`,
  `mfg:XXXX` (company ID en hex), `svc`, `unknown-weak`.
- Los SCAN_RSP se ignoran (con scan pasivo no llegan).

Por identidad se guardan RSSI mínimo/máximo/promedio, primera y última vez, MAC observada, categoría y
nombre. Están en pantalla y en `ble_identities` del JSON de metadatos.

## Teclas

| Tecla | Acción |
|---|---|
| `enter` | iniciar / detener la captura. Al iniciar, abre una sesión si no hay una |
| `b` `w` `l` | armar / desarmar BLE, WiFi (solo diagnóstico) y LoRa |
| `g` | prender / apagar el GPS |
| `r` | cerrar la sesión actual (limpio) y abrir una nueva |
| `s` | detener, guardar y cerrar la sesión |

Al salir de la app, la sesión también se cierra limpio.

## Archivos (en `/sdcard/wardriving/sessions/`)

- `session-XXXX.csv.part`: CSV append-only mientras la sesión está abierta. Las filas se escriben por
  lote (cada 5 s o al juntar 32 filas). Cada lote se abre en modo `ab`, se escribe, se hace `fflush` y se
  cierra. Si se corta la energía, este archivo es la fuente de verdad.
- `session-XXXX.csv`: el `.part` renombrado al cierre limpio.
- `session-XXXX.geojson`: solo al cierre limpio (RFC 7946 `FeatureCollection`). Se escribe en
  `.geojson.tmp` y se renombra al terminar, así nunca queda un GeoJSON a medias con el nombre final.
- `session-XXXX.json`: metadatos de la sesión: canal LoRa, backends, límites, contadores, resumen por
  identidad BLE y `boot_unix_ms`.

**Recuperación:** al arrancar, cada `session-XXXX.csv.part` que no tenga su `.csv` se convierte en
`.csv` (solo las filas completas y válidas) y `.geojson`. El `.part` queda intacto. La app nunca borra
ni formatea nada. Si algo falla a mitad de camino, quedan los `.tmp`.

**Error de SD:** se deja de escribir (`log OFF` y `ERROR SD` en pantalla), pero la captura sigue en RAM.

### CSV

UTF-8, WGS84 en grados decimales con 7 decimales, una fila por muestra agregada (no por advertising):

```
timestamp_ms,source,identity,mac_or_addr,rssi_dbm,snr_db,frequency_hz,channel,name,category,lat,lon,alt_m,accuracy_m,satellites,gps_valid,payload_hex
```

- Cada identidad escribe una fila al verla por primera vez. Después escribe una por ventana de 5 s con el
  RSSI (y el SNR, en LoRa) promedio de la ventana, solo si hubo observaciones.
- `timestamp_ms`: millis desde el arranque del equipo. La hora UTC es `boot_unix_ms + timestamp_ms`
  (`boot_unix_ms` está en el JSON cuando el GPS dio la hora; si no, es `null`).
- Sin fix, o con un fix de más de 3 s: `gps_valid=false` y lat/lon/alt/accuracy vacíos. La fila se
  registra igual, marcada. La edad del fix está en RAM, pero no es columna: el encabezado es fijo.
- `accuracy_m` es una estimación: HDOP × 5 m.
- RFC 4180: un campo con `,` `"` CR/LF o espacios en los bordes va entre comillas, con `""`. Un nombre
  que una planilla tomaría por fórmula (`=` `+` `-` `@` tab al principio) lleva un `'` delante. Los
  nombres BLE vienen saneados: UTF-8 válido, control → espacio, a lo sumo 24 bytes.
- `payload_hex` (LoRa): hasta 48 caracteres (24 bytes) del último paquete de esa identidad al momento de
  escribir el lote.
- Campos que no aplican: vacíos (BLE no tiene `snr_db` ni `frequency_hz`, porque la ABI no da el canal de
  advertising).

### GeoJSON

Hay un `Feature` por fila, con geometría `Point` y `coordinates: [lon, lat, alt]`. En `properties` van el
resto de las columnas (vacío = `null`). Una fila sin fix tiene `geometry: null` (RFC 7946 §3.2): no se
inventan puntos.

## Límites (v1)

48 identidades BLE, 32 LoRa (WiFi: 32 reservadas, sin backend), 32 filas pendientes en RAM, payload hex
≤ 48, nombre ≤ 24 bytes y línea CSV ≤ 320 bytes. Lo que no entra se descarta y se cuenta en pantalla
(`drop`) y en el JSON.

## Memoria (v0.1.0, `readelf -l dist/wardriving-v0.1.0.elf`)

| | medido | presupuesto |
|---|---|---|
| `.text` (LOAD R E) | 0x5860 = 22 624 B | 64 KB |
| datos (LOAD RW, MemSiz) | 0x3540 = 13 632 B | 18 KB |

Las tablas y el ring son arrays fijos (sin heap, STL, `String`, excepciones ni RTTI). El ring guarda
referencias a la tabla, y la identidad y la categoría se recalculan desde la clave.

`app.json` incluye `../meshtastic/src` para `gnss.h`. `build_app.py` busca palabras clave en todo ese
directorio y encuentra `ble_srv` en `apps/meshtastic/src/main.cpp`, así que el header queda con
**ABI minor 8** aunque la app no use el servidor GATT. Funciona con el OS actual (ABI 1.8).

## Tests

```
c++ -std=c++17 -Wall -Iapps/wardriving/src -Iapps/wardriving/test/stub -Iapps/ble-sentinel/src \
    apps/wardriving/test/core_test.cpp apps/ble-sentinel/src/fingerprint.cpp -o /tmp/wd_test && \
    /tmp/wd_test apps/wardriving/src/main.cpp
```

El test cubre el escape CSV (coma, comillas, newline y fórmulas), los campos vacíos, las coordenadas
WGS84 y sus bordes, el orden `[lon,lat]`, la paridad del fingerprint con Sentinel y la identidad estable
ante rotación de MAC. También cubre los límites de tablas, ring, payload, nombre y línea, la recuperación
de un `.part` cortado y la revisión de que el código solo escucha. Los advertisings y paquetes del test
están armados a mano para ejercitar el parser: la app no simula datos de radio.

No se probó en hardware.
