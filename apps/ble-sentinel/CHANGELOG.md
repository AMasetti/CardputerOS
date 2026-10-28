# Changelog

## [0.11.0]
- Nuevo: detección real de drones (`Category::DroneRemoteId`) -- hasta
  ahora la categoría existía pero nada la poblaba. Se matchea por
  presencia del UUID `0xFFFA` en el AD type Service Data (0x16) de los
  advertising packets, que el Bluetooth SIG asigna explícitamente a
  "ASTM Remote ID" (confirmado contra el registro público oficial,
  `assigned_numbers/uuids/sdo_uuids.yaml`, no un proyecto de terceros).
  No decodifica el mensaje ODID (Basic ID/Location/Self-ID/etc.), solo
  detecta presencia -- mismo nivel de profundidad que el resto de las
  firmas v1. `ble_scanner`/`fingerprint` ganan un campo `remoteIdService`
  nuevo (mezclado en el fingerprint hash); `resolve()` en `main.cpp` lo
  chequea antes que nada más, porque es la señal más específica
  disponible. Un dispositivo Remote ID típicamente no trae manufacturer
  data ni service UUIDs genéricos -- antes eso lo marcaba
  `confidenceLow`, ahora `remoteIdService` por sí solo basta para no
  bajar la confianza.
- Nuevo: reiniciar la sesión de prueba sin reiniciar el dispositivo,
  tecla `r` -- pedido explícitamente para poder repetir pruebas de campo
  rápido. Limpia todo lo visto hasta ahora (`sighting_store::
  resetSession()`) y el estado de `ConvoyAnalyzer`; si hay log a SD
  prendido, cierra el archivo de historial actual y abre uno nuevo bajo
  un `session_id` incrementado, para no mezclar una corrida con la
  siguiente en el mismo archivo. Lo marcado como "mío" (`trusted`) NO se
  toca -- no debería haber que remarcar el propio AirTag en cada reset.
  Deliberadamente sin confirmación (una tecla, un efecto, mismo criterio
  que `c`/`e`/`l` -- el hardware no tiene diálogo modal).
- Decisión explícita, no implementada a propósito: **no** se agrega
  persistencia de configuración (mute/volumen/log-a-SD) entre reinicios
  reales del dispositivo -- pedido así por el usuario. Sigue arrancando
  en los defaults de siempre (mute, log apagado) cada power-on; el reset
  de sesión de arriba es la herramienta para "empezar de nuevo" dentro
  de la misma sesión, no para sobrevivir un apagado.

## [0.10.1]
- Fix: en 0.10.0, "identificado" (para color y para orden de la lista)
  seguía siendo solo `category != Unknown` -- un dispositivo que anuncia
  su propio nombre (ej. AirPods) pero no matchea ninguna firma de
  SignatureDB quedaba igual que un unknown de verdad: pintado blanco y
  enterrado al fondo de la lista por RSSI nomás, aunque ya se le conociera
  el nombre. `isIdentified()` y `categoryColor()` ahora también cuentan
  "tiene nombre anunciado" como identificado -- se pintan verde y ordenan
  junto con el resto de lo identificado, no con los unknowns reales. El
  audio no cambia: `Category::Unknown` sigue siendo `SuspiciousDefault`
  tenga nombre o no, un nombre anunciado no prueba nada por sí solo.
- **No es un bug, es una limitación ya documentada:** AirPods y Beats Pro
  (reportados por el usuario) no aparecen en absoluto mientras están
  conectados por audio a un teléfono -- usan **Bluetooth Classic (A2DP)**
  para el audio, no BLE, y mientras están conectados por Classic no
  tienen razón para seguir anunciando por BLE. `BLEScanner` es
  observer-only de BLE (NimBLE); Bluetooth Classic es una pila de radio
  distinta (Bluedroid) que el ESP32 no puede correr al mismo tiempo sin
  reconfiguración de bajo nivel. Ver sección 4 de la spec -- queda fuera
  de alcance por ahora, mismo motivo que WiFiSniffer.

## [0.10.0]
- Fix real: la lista principal se reordenaba sola entre dispositivos sin
  que nada relevante cambiara -- el comparador de `sortedOrder()` en
  `main.cpp` terminaba en "por RSSI desc" sin ningún criterio para romper
  empates, y `std::sort` no es estable: con RSSI real saltando varios dBm
  por paquete, dos dispositivos con RSSI igual (o que se cruzan de un
  paquete al siguiente) podían volver en cualquier orden relativo cada
  vez. Antes era invisible porque todo lo no identificado decía
  "unknown" igual (no se notaba cuál era cuál); en cuanto 0.9.0 empezó a
  mostrar el nombre real, ese reordenamiento sin sentido se volvió
  visible ("los dispositivos con nombre van de arriba a abajo"). Tiebreak
  final agregado: orden de inserción (primera vez visto en la sesión),
  determinístico.
- Nuevo: registro general de fabricantes (company identifiers oficiales
  del Bluetooth SIG, no solo las firmas de amenaza existentes) -- ~60
  entradas curadas a mano de electrónica de consumo reconocible (Samsung,
  Google, Garmin, Sony, Bose/JBL, Casio, Seiko, Sonos, routers, etc.) más
  cámaras de acción (GoPro, Insta360) y robots/drones (DJI, iRobot, Anki,
  Ninebot). El label de la lista principal ahora prioriza: nombre
  anunciado > fabricante (si matchea algo del registro) > categoría
  genérica -- antes, sin nombre anunciado, siempre caía en "unknown"
  aunque el company ID fuera identificable.
- Nuevo: código de colores explícito por categoría (antes solo
  rojo=sospechoso vs. gris=auricular vs. ámbar=sin verificar): **blanco**
  = unknown, **azul** = smart glasses/cámaras de acción ("meta o
  filmación"), **violeta** = drones y robots, **rojo** = Find My,
  **verde** = otro dispositivo identificado (auricular, o vendor
  genérico del registro -- "todo tranquilo"). Cambio de comportamiento
  visual deliberado: antes Unknown se pintaba rojo ("tratado como
  sospechoso por defecto"); ahora es blanco/neutro. El audio NO cambió --
  `AlertProfile::SuspiciousDefault` sigue siendo el default para Unknown,
  solo cambia el color en pantalla. Los nuevos matches de fabricante
  genérico (`Category::IdentifiedVendor`) usan `AlertProfile::Silent` --
  no tiene sentido que un Garmin o un router suenen igual que un
  dispositivo real sin identificar.

## [0.9.0]
- Fix: la lista principal mostraba "unknown" aunque el dispositivo sí
  anunciara un nombre (ej. relojes Casio con BLE) -- ese nombre solo se
  veía al entrar a Detalle. La fila ahora muestra el nombre anunciado
  cuando lo hay (hasta 12 caracteres), y recién si no hay nombre cae a la
  categoría de SignatureDB. Es solo el label de la fila: el color/franja
  de categoría sigue siendo el de SignatureDB (un nombre no verifica qué
  tipo de dispositivo es). Como se lee de `lastLocalName` en cada render,
  si el nombre llega en un paquete posterior (scan response, no el primer
  advertising) se corrige solo en el próximo redibujado, sin lógica extra.
- Nuevo: log continuo a SD ahora es **opt-in, apagado por defecto** --
  antes se grababa automáticamente en cuanto había tarjeta montada, sin
  forma de desactivarlo. Toggle con la tecla `l`; el chip de estado (antes
  "SD OK"/"SD --") ahora muestra "LOG ON"/"LOG OFF"/"SD --" para que se
  vea la diferencia entre "hay tarjeta" y "estoy grabando". Apagar el log
  no borra ni afecta nada de `sighting_store` en RAM -- solo el archivo.
- Nuevo: la lista principal ahora solo muestra dispositivos "activos"
  (vistos en los últimos 10s) -- antes se acumulaba todo lo visto en la
  sesión, incluidos dispositivos de los que el usuario ya se alejó hace
  rato. El filtro es solo de UI: `sighting_store` sigue el Record para
  toda la sesión (lo necesitan Detalle/Seguimiento) y el log a SD (si está
  prendido) sigue registrando todo lo que pasa por `record()`, visible en
  la lista o no -- un dispositivo puede "desaparecer" de la pantalla sin
  dejar de loggearse. El filtro no aplica con el scan pausado (pausar es
  para elegir de la lista, no para que se vacíe sola mientras la mirás).

## [0.8.1]
- Fix: la burbuja de proximidad usaba el rango teórico de BLE (-95dBm a
  -40dBm) para mapear la señal a tamaño. En hardware real (Cardputer +
  ESP32-S3) ese rango nunca se alcanza: el usuario reportó -60dBm al lado
  del dispositivo y -80dBm lejos, muy lejos de los extremos teóricos --
  resultado, la burbuja nunca llegaba a llenarse ni a vaciarse del todo,
  pasaba casi todo el tiempo a mitad de tamaño sin importar la distancia
  real. Recalibrado a -85dBm (lejos, fracción 0.0) / -55dBm (al lado,
  fracción 1.0), con margen de 5dBm a cada lado sobre lo reportado para no
  pegar en el tope/piso con el primer paso. El tono de audio continuo
  usaba su propio rango hardcodeado (-95/-40) para la frecuencia -- ahora
  comparte el mismo rango calibrado que la burbuja, para no contar dos
  historias distintas de "qué tan cerca" para la misma señal.
- **Limitación conocida:** este rango está calibrado a ojo con un solo
  reporte de campo, no medido con un set de dispositivos/distancias. TX
  power y antena varían por dispositivo rastreado (no todos son iguales),
  así que -55/-85 es una aproximación razonable, no una calibración
  exacta. Si en uso real sigue sin llenarse/vaciarse del todo con otros
  dispositivos, estos valores son candidatos a ajustar de nuevo.

## [0.8.0]
- Rediseño de la pantalla de Seguimiento: la clasificación discreta
  "acercándose/estable/alejándose" (v1 con split de ventana cruda, v2 en
  0.7.1 con cruce de EMA) seguía siendo errática en uso real -- parado
  quieto al lado del propio dispositivo, alternaba entre los tres estados.
  Causa de fondo: cualquier umbral categórico sobre una señal con 10-15dBm
  de ruido normal de RF SIEMPRE va a tener casos límite que parpadean, no
  importa cuánto se ajuste la zona muerta -- no era un bug de calibración,
  era el enfoque equivocado.
- Reemplazado por una burbuja continua que crece/achica con la señal
  filtrada (una tercera EMA propia, `proximityFraction()`, más lenta que
  las de debug), estilo "Find Nearby" de Apple para AirTags: un anillo
  exterior fijo marca el "objetivo", la burbuja interior se acerca a su
  tamaño a medida que la señal mejora, y se pone verde al estar muy cerca.
  Sin estados discretos no hay nada que "parpadee" categóricamente -- en
  el peor caso la burbuja tiembla un poco, que es honesto (la señal de
  verdad tiembla).
- El número grande + sparkline + texto de tendencia se sacaron de la UI
  principal; el dBm crudo queda chico y abajo, solo para debug.
  `trend()`/`smoothedRssi()` en `room_sweep.h` ahora están documentados
  explícitamente como debug-only.

## [0.7.1]
- Fix: la tendencia en la pantalla de Seguimiento era errática (decía
  "acercándose" lejos del dispositivo y viceversa). Causa real: RSSI de
  BLE salta 10-15dBm aunque el dispositivo esté quieto y cerca (es física
  de RF normal, no un bug), pero el cálculo comparaba apenas 4 muestras
  crudas contra las otras 4 con una zona muerta de solo 3dBm -- un solo
  paquete ruidoso alcanzaba para voltear el resultado. Reemplazado por
  dos promedios móviles exponenciales (rápido/lento) y zona muerta sobre
  la diferencia entre ambos: hace falta una tendencia sostenida de varios
  paquetes para mover la aguja, no uno solo.
- El número grande (`smoothedRssi()`) ahora muestra la EMA rápida en vez
  de un promedio simple de ventana -- reacciona más parejo.

## [0.7.0]
- Nuevo: detección de accesorios Find My de Apple (AirTag y similares).
  Company ID de Apple (0x004C) solo no alcanza -- todo dispositivo Apple
  lo usa -- así que se capturó el byte de sub-protocolo del manufacturer
  data (`mfgTypeByte`) y se matchea `0x12` (Find My) específicamente.
  Categoría `FindMyTracker` nueva.
- Nuevo: dispositivos marcados como "mío" (tecla `t` en Detalle), con
  persistencia a SD (`/sentinel/trusted.txt`, sobrevive un reinicio). Un
  dispositivo propio no dispara alertas ni cuenta como sospechoso de
  convoy -- necesario porque tu propio AirTag está siempre con vos y por
  definición parecería "seguirte" igual que lo haría uno ajeno; la app
  solo es útil contra rastreo de terceros si le decís cuál es el tuyo.
  Se ordenan al final de la lista (ya los conocés, no urgen).
- Fingerprint ahora incluye `mfgTypeByte` para no colisionar distintos
  sub-protocolos de un mismo fabricante (ej. AirTag vs. AirPods, ambos
  Apple/0x004C).

## [0.6.1]
- Fix real: `category`/`confidenceLow` de un `Record` solo se calculaban
  en el primer paquete y nunca se actualizaban después. Muchos
  dispositivos mandan el nombre recién en el scan response, no en el
  primer advertising -- si ese primer paquete no traía nombre, el
  dispositivo quedaba clasificado como `Unknown` para siempre, aunque
  packets posteriores sí revelaran quién era. `resolve()` ahora mezcla
  el nombre/company ID del paquete de hoy con lo que el `Record` ya
  sabía de antes, y `sighting_store::record()` persiste la categoría
  recalculada en cada paquete, no solo al crear el registro.

## [0.6.0]
- Scroll en la lista: con más dispositivos que filas visibles, `cursorIndex`
  pasa a ser un índice absoluto sobre toda la lista (no solo las filas en
  pantalla) y la ventana visible se desliza para seguirlo. Scrollbar a la
  derecha (track + thumb) cuando hay más filas que lugar.
- Prioridad de orden actualizada: sospechosos de convoy, después
  **resultados identificados** (categoría real de SignatureDB -- son los
  interesantes, se destacan sobre lo no identificado aunque tenga mejor
  señal), recién ahí el resto por RSSI desc dentro de cada grupo.

## [0.5.0]
- Se reemplazó el radar por una pantalla de seguimiento honesta con lo
  que el RSSI realmente da: número grande + sparkline de las últimas
  muestras + texto de tendencia. El radar sugería dirección/posición que
  nunca tuvimos (ángulo fijo o no); esto es una sola dimensión y se lee
  como tal.
- Nuevo: tono de audio continuo opcional en la pantalla de seguimiento
  (frecuencia sube cuanto más cerca, estilo detector de metales), para
  buscar sin mirar la pantalla. Toggle con G0 corto (mantenido sigue
  siendo el mute global, sin cambios ahí).
- `Screen::Radar` renombrado a `Screen::Follow` / `drawRadar()` a
  `drawFollow()` en el código, para que el nombre no siga sugiriendo lo
  que ya no hace.

## [0.4.0]
- Nuevo: pantalla de Detalle entre la lista y el radar. Flujo ahora es
  Lista -> (enter) -> Detalle -> (enter) -> Radar, `del` vuelve un paso
  atrás en cada una.
- Detalle muestra fabricante (si hay firma conocida; heurística o
  "desconocido" si no), company id, nombre anunciado, RSSI actual, TX
  power declarado, tamaño de payload, veces visto, rotaciones de MAC,
  ventana temporal y score de ConvoyAnalyzer.
- `sighting_store::Record` ahora guarda el company id / TX power /
  payload length / nombre local del último advertising -- antes se
  resolvían y se perdían, no quedaban disponibles para mostrar después.

## [0.3.0]
- Nuevo: selección en la lista con el scan pausado (`;`/`.` mover,
  `enter` elegir) y pantalla de radar (`RoomSweep`) para seguir un
  dispositivo puntual vía RSSI -- ángulo fijo por fingerprint, solo la
  distancia al centro varía con la señal, con indicador de tendencia
  (acercándose/alejándose/estable). `del` vuelve a la lista.
- Nuevo: exportación a SD con la tecla `e` -- CSV + JSON del
  sighting_store actual, con hash SHA-256 de integridad al lado de cada
  archivo y un log de exports.
- Pendiente, no implementado todavía: WiFiSniffer (correlación BLE+WiFi)
  y soporte de RTC por Grove -- motivos documentados en la spec.

## [0.2.0]
- Nuevo: ConvoyAnalyzer (detección de seguimiento sostenido). Tecla `c`
  marca "cambié de lugar"; un dispositivo visto en 3+ lugares distintos
  (+ duración sostenida y/o ventana temporal larga) se marca `!SIGUE`,
  sube al tope de la lista y dispara un perfil de audio propio
  (`ConvoySuspect`), más agudo y con cooldown más corto.
- Fix: el cooldown de `AlertEngine` era solo por dispositivo, no por
  (dispositivo, perfil) — una alerta nueva podía quedar bloqueada por el
  cooldown de una alerta distinta para el mismo fingerprint.

## [0.1.2]
- Fix: la lista se reordenaba y parpadeaba en cada paquete de advertising
  (varias veces por segundo con varios dispositivos cerca). Render
  throttleado a ~300ms, sin perder ninguna actualización.
- Nuevo: `space` pausa/reanuda el scan (antes no había forma de detenerlo).

## [0.1.1]
- UI rehecha con el mismo lenguaje visual que `shell-rc`/`led-rc`: HUD
  arriba, chips de estado (audio/SD), barras de señal por dispositivo,
  franja de color por categoría, footer con atajos.
- Audio arranca en mute por defecto; se activa/desactiva con G0 mantenido
  (~0.8s), ya no con la tecla `m`.

## [0.1.0]
- Primera versión: scan BLE pasivo (observer-only, forzado a nivel de build).
- Fingerprint de dispositivo resistente a rotación de MAC (company ID +
  forma del payload + TX power + service UUIDs).
- SignatureDB inicial: smart glasses (Meta/Luxottica, Snap) por company ID,
  heurística de auriculares por nombre.
- Alertas de audio por categoría (sospechoso a volumen pleno, auricular a
  volumen bajo, confiado en silencio) con cooldown por dispositivo.
- Registro de avistamientos con log a SD, timestamp relativo al boot
  (sin RTC todavía -- pendiente módulo Grove).
- Pendiente: ConvoyAnalyzer, RoomSweepEngine, WiFiSniffer, ExportModule.
