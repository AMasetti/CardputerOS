# Brief: wardriving pasivo multirradio para CardputerOS

## Alcance
Crear `apps/wardriving`, una app de escucha pasiva para Cardputer-ADV que releve dispositivos/señales BLE, WiFi y LoRa junto con la posición del GNSS del hat. No debe conectarse, autenticarse, asociarse, transmitir, desautenticar ni intentar descifrar tráfico. No reemplazar ni editar apps existentes ni los briefs `BRIEF-v3.md`/`BRIEF-v4.md` ni `apps/skypointer-lite`.

La app parte del modelo de identidad del BLE Sentinel: fingerprint estable por manufacturer data/company ID, longitud, TX power y UUIDs; MAC solo como dato efímero. Reutilizar código pequeño y explícito desde `apps/ble-sentinel/src` mediante `include` si el build lo permite; no traer NimBLE, ArduinoJson, STL pesado ni otra librería grande al ELF.

## Decisiones de v1

- Tres fuentes seleccionables e independientes: BLE, WiFi y LoRa.
- BLE: observer pasivo, no advertising, no conexión; agrupar por fingerprint BLE Sentinel y conservar RSSI mínimo/máximo/promedio, primera/última muestra, MAC observada, fabricante/categoría y nombre si existe.
- GNSS: reutilizar `apps/meshtastic/src/gnss.h`; cada muestra debe llevar latitud, longitud, altitud, precisión/fix, satélites y edad del fix. No registrar puntos sin fix salvo que se marquen explícitamente como `gps_valid=false`.
- WiFi: v1 puede usar el escaneo pasivo que la ABI actual permita sin asociarse. Si para beacon/probe promiscuo hace falta una API de OS nueva, no inventar un símbolo: dejar un backend `unsupported` con diagnóstico visible y documentar la extensión mínima de ABI requerida. No usar el escaneo STA como si fuera sniffing de paquetes sin aclararlo.
- LoRa: recepción pasiva solamente, en una configuración explícita y visible (por defecto Argentina 915 MHz si el servicio/hat lo soporta). Registrar RSSI/SNR, frecuencia y payload como hex truncado; no interpretar ni retransmitir paquetes ajenos. No hacer barrido ciego de bandas en v1: el SX1262 actual recibe un canal por vez.
- Muestreo: no escribir cada evento a la SD. Mantener una tabla acotada de dispositivos/señales y un ring buffer pequeño de muestras. Vaciar por lote cada 5 s o al alcanzar 32 registros, lo que ocurra primero. Al cerrar lote: escribir, `fflush`, cerrar y reabrir si hace falta; ante error detener logging pero conservar la captura en RAM.
- Robustez SD: cada sesión escribe un CSV de eventos append-only en `/sdcard/wardriving/sessions/session-XXXX.csv.part`. Al cierre limpio se cierra y renombra a `.csv`; el `.part` debe ser legible y recuperable si se corta la energía. Además generar un `session-XXXX.json` de metadatos y, si es barato y seguro, un GeoJSON final para importación directa. Nunca borrar ni formatear.
- Formato primario portable: CSV UTF-8, WGS84 decimal, una fila por muestra agregada (no por advertising), con encabezado estable:
  `timestamp_ms,source,identity,mac_or_addr,rssi_dbm,snr_db,frequency_hz,channel,name,category,lat,lon,alt_m,accuracy_m,satellites,gps_valid,payload_hex`
  `source` ∈ `ble,wifi,lora`; campos no aplicables vacíos; `identity` nunca debe depender solo de una MAC rotatoria.
- Formato cartográfico: GeoJSON RFC 7946 `FeatureCollection`, `Point`, `[lon,lat]`, properties con el resto de columnas y `source`. Si la sesión termina abruptamente, el CSV `.part` es la fuente de verdad; no declarar GeoJSON válido hasta cierre limpio.
- UI mínima: estado de cada radio, fix GNSS, cantidad de identidades, muestras en RAM, logging ON/OFF, lote pendiente y diagnóstico de backend. Teclas: `b` BLE, `w` WiFi, `l` LoRa, `g` GPS/log, `r` nueva sesión, `s` guardar/cerrar sesión, `enter` iniciar/detener. Sin mapa falso en pantalla: mapa se ve en GIS externo.

## Memoria y límites

- Presupuesto objetivo: `.text <= 64 KB`, `data MemSiz <= 18 KB`, medidos con `readelf -l` del ELF. No usar `std::vector`, `String`, excepciones, RTTI ni tablas grandes en el camino principal si pueden reemplazarse por arrays fijos y buffers de SD.
- Máximos v1: 48 identidades BLE, 32 WiFi, 32 LoRa, 32 muestras pendientes, payload hex <= 48 caracteres, nombre <= 24, línea CSV <= 320 bytes. Lo excedente se descarta con contador visible.
- El código debe depender solo de `sdk/include/cpos/abi.h` y headers compartidos pequeños. Permisos: `ble`, `wifi`, `lora`, `gnss`, `sd`.

## Pruebas y aceptación

1. Crear manifest, README, CHANGELOG, fuente y tests host de parser/formateador CSV/GeoJSON.
2. `python3 sdk/tools/build_app.py apps/wardriving` termina correctamente.
3. Inspeccionar `readelf -l dist/wardriving-v*.elf` y reportar `.text`/`MemSiz` dentro del presupuesto o bloqueo explícito.
4. Test host verifica: coma/quote/newline en nombres, campos vacíos, coordenadas WGS84, orden `[lon,lat]`, identidad BLE estable ante rotación de MAC, límites de buffers y recuperación del CSV `.part`.
5. Documentar claramente que WiFi promiscuo puede quedar `unsupported` con ABI actual y que LoRa requiere canal/configuración; no simular datos de radio.
6. No compilar el OS, flashear, committear, pushear ni publicar sin indicación posterior.
7. Al finalizar, inspeccionar `git diff` y `git status`; dejar intactos todos los cambios preexistentes no relacionados.

## Archivos de referencia

- `sdk/include/cpos/abi.h` (ABI real; WiFi actual solo scan STA, BLE observer, LoRa RX y GNSS)
- `apps/ble-sentinel/src/fingerprint.*`, `signature_db.*`, `sighting_store.*`
- `apps/meshtastic/src/gnss.h`
- `docs/architecture.md`
- `references/cardputer-os-ram-budget.md` de la skill hardware
