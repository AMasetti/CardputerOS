# Brief v2: interfaz clara para wardriving pasivo

Todo lo anterior de `BRIEF-wardriving-v1.md` sigue vigente. Este brief agrega una corrección de interfaz basada en la prueba en hardware y la foto de la pantalla.

## Problema observado

La pantalla actual comprime demasiada información en una línea: `REC`, `on`, `off`, `unsupported`, contadores y parámetros quedan mezclados. No se puede responder mirando un segundo:

1. ¿La captura está corriendo o detenida?
2. ¿BLE está escuchando ahora?
3. ¿LoRa está escuchando ahora?
4. ¿WiFi está disponible o deliberadamente no disponible?
5. ¿Hay GPS con fix válido?
6. ¿La SD está grabando o hubo error?
7. ¿Qué tecla hace cada acción?

## Objetivo de la nueva pantalla

En 240x135, usar el lenguaje visual y la claridad del BLE Sentinel existente: fondo oscuro, filas separadas, colores con significado estable y estados textuales. No mostrar parámetros técnicos largos en el dashboard. Los detalles van a una segunda pantalla.

### Dashboard obligatorio

La pantalla principal debe tener esta estructura, adaptada al ancho real y usando fuente fija legible:

```text
● CAPTURA       00:12:48
Sesión 0007   SD: GRABANDO

BLE   ESCUCHANDO   24 ids  128 ev
WiFi  NO DISP      ABI 1.8
LoRa  ESCUCHANDO    7 ids   43 ev
GPS   FIX           8 sat  ±5m

Último: BLE       hace 2s
Muestras: 171     lote: 12

ENTER pausa  B BLE  L LoRa
R nueva       S guardar  H ayuda
```

La maqueta es conceptual: ajustar textos para que ninguna línea se corte. Priorizar siempre el nombre del estado sobre el contador.

### Reglas de estado

- El encabezado global debe decir exactamente `CAPTURA` + `ACTIVA`, `PAUSADA` o `DETENIDA`; no usar `REC` ni `PAUSA` solos.
- Cada radio tiene una fila con estado textual inequívoco:
  - BLE: `ESCUCHANDO`, `APAGADO` o `ERROR`.
  - WiFi: `NO DISP` con motivo corto `ABI 1.8`; no mostrarlo como `OFF` ni como si estuviera capturando.
  - LoRa: `ESCUCHANDO`, `APAGADO` o `ERROR`; usar `RX` solo como dato secundario.
  - GPS: `FIX`, `BUSCANDO`, `SIN FIX`, `APAGADO` o `ERROR`.
- No usar `on/off` como estado principal. No usar un asterisco para decir armado.
- BLE y LoRa se describen como `ESCUCHANDO`, nunca `CONECTADO`: son radios pasivas.
- `SD: GRABANDO` significa que hay sesión abierta y el último lote escribió correctamente. `SD: RAM` significa que la captura sigue pero no se está guardando. `SD: ERROR` debe ser rojo y mostrar motivo en la pantalla de ayuda/diagnóstico.
- GPS válido significa fix fresco según `WD_FIX_MAX_AGE_MS`; `GPS` presente o puerto abierto no equivale a fix.
- El contador `ev` es eventos recibidos durante la sesión; `ids` son identidades agregadas. Mantener esas etiquetas siempre.
- Cada radio debe mostrar su última actividad en una vista de detalle; en el dashboard basta con el estado y contadores.

### Colores

Conservar una convención similar al Sentinel, pero redundante con texto:

- rojo: captura activa / error crítico / SD error;
- verde: escuchando o GPS con fix;
- amarillo: pausada, buscando, sin fix o RAM;
- gris: apagado/no disponible;
- celeste: ayuda, última actividad o navegación.

No depender solo del color.

## Controles

Los controles visibles deben coincidir con el comportamiento real y estar partidos en dos líneas:

- `ENTER`: iniciar / pausar captura.
- `B`: activar / apagar BLE.
- `L`: activar / apagar LoRa.
- `G`: activar / apagar GPS.
- `R`: cerrar sesión actual y abrir una nueva; si estaba capturando, continúa capturando.
- `S`: detener, guardar y cerrar sesión.
- `H`: abrir/cerrar ayuda y diagnóstico.

La ayuda debe ocupar una pantalla propia, con una acción por línea:

```text
CONTROLES
ENTER  iniciar / pausar
B      BLE escuchar
L      LoRa escuchar
G      GPS on / off
R      nueva sesión
S      guardar y cerrar
H      volver
```

Si `?` ya es más accesible que `H` en el layout real, se puede aceptar como alias, pero `H` debe funcionar.

## Vista de ayuda/diagnóstico

Mostrar en la segunda pantalla:

- estado de captura y sesión;
- BLE: `ESCUCHANDO PASIVO`, ids, eventos, descartes;
- WiFi: `NO DISPONIBLE`, motivo `ABI 1.8 sin monitor pasivo`;
- LoRa: `RX`, frecuencia, SF, BW, sync;
- GPS: puerto, NMEA, fix, satélites, edad;
- SD: `GRABANDO`, `RAM` o `ERROR`, archivo de sesión;
- versión de app.

No mezclar este detalle con el dashboard.

## Restricciones de implementación

- No modificar la lógica pasiva ni agregar ninguna transmisión.
- No agregar librerías, canvas extra, heap dinámico ni fuentes grandes.
- Mantener el canvas de pantalla provisto por el OS; no crear un framebuffer adicional.
- Reusar helpers de dibujo del Sentinel solo si no aumenta el ELF fuera del presupuesto.
- Mantener `.text <= 64 KB` y datos `MemSiz <= 18 KB`.
- Bump de versión a `0.2.0` en `app.json` y `CHANGELOG.md`.
- Actualizar README con los controles y estados reales.
- Agregar/actualizar tests host para verificar que no vuelvan `REC`, `on/off`, asteriscos como estado y que estén presentes las etiquetas `CAPTURA`, `ESCUCHANDO`, `NO DISP`, `FIX`, `SD` y controles.

## Aceptación

1. Compilar tests host: todos pasan.
2. Compilar `python3 sdk/tools/build_app.py apps/wardriving`.
3. Medir ELF con `readelf -l`; cumplir presupuesto.
4. Inspección estática confirma que siguen ausentes conexiones, asociaciones, sockets, transmisión LoRa y scan WiFi activo.
5. `git diff` muestra solo cambios de `apps/wardriving` y este brief; no tocar `skypointer-lite`, briefs anteriores ni `os/.cache`.
6. No declarar verificación de legibilidad en hardware hasta probar la nueva versión en la Cardputer.
