# Fase 1 — Investigación

Estado: **borrador para tu revisión**. Sin código. Todo lo de acá alimenta `docs/architecture.md` (Fase 2).

## 0. Cómo leer este documento

Cada afirmación técnica lleva una marca de confianza:

| Marca | Significa |
|---|---|
| **[V]** | Verificada en una fuente que leí en esta sesión (URL en §13). |
| **[L]** | Verificada leyendo tus propios repos (`Cardputer-ADV-Apps`, `CardputerADV-Launcher`). |
| **[C]** | Conocimiento previo mío, **no verificado** en esta sesión. Hay que validarlo en la Fase 3 o contra la doc oficial antes de diseñar sobre eso. |

Limitaciones de esta investigación, para que no queden escondidas:

- Los hosts `docs.espressif.com`, `components.espressif.com` y `developer.flipper.net` están **bloqueados** en la red del contenedor. No pude leer la doc de memoria/MMU de ESP-IDF, el README del componente `elf_loader` ni la doc de Flipper en su sitio. Usé `raw.githubusercontent.com` y resultados de búsqueda como sustituto.
- El fetch de páginas devuelve resúmenes de un modelo chico, no el texto crudo. No leí el código fuente de Tactility, Flipper ni elf_loader (lo cual, para clean-room, tampoco es un problema: aprendo la arquitectura, no el código).
- No leí el cargador de Tactility. **No sé** si usa `espressif/elf_loader` o uno propio.
- No tengo placa en el contenedor. Ningún número de RAM/tiempo de este documento es medido.

Si querés cerrar esos huecos antes de la Fase 2, habilitá esos tres dominios en la red del entorno y los reviso. Si no, sigo con las marcas **[C]** y la Fase 3 los resuelve.

## 1. Resumen: qué cambia o se agrega al diseño

Las decisiones de arquitectura que me diste se sostienen. La investigación encontró **siete cosas que hay que decidir con datos** y que el enunciado no resolvía:

1. **El modo (a) "ELF en SRAM" choca con la protección de memoria de IDF y con el heap ejecutable [C].** El ESP32-S3 ejecuta código desde SRAM solo por el alias de instrucciones (IBUS), y el sistema de permisos de IDF (`memprot`/PMS) puede prohibirlo en regiones que son datos. Además, el heap `MALLOC_CAP_EXEC` es solo lo que sobra del linker script. Probablemente haya que **reservar una arena de código con tamaño fijo en el linker**, y eso se paga en heap. Es el riesgo n.º 1 (R1).
2. **El modo (b) "XIP desde el slot" no es solo "mapear y saltar" [C/V].** `esp_mmu_map` elige la dirección virtual (o la podés fijar con `esp_mmu_map_virt`, según el resumen de la doc **[V]**). Xtensa carga direcciones absolutas desde literal pools, así que o el código es PIC (`-fPIC`, vía GOT en RAM), o se linkea a una ventana virtual fija. Ver §5.3 y R2.
3. **Las ISR y todo lo que corre con el cache deshabilitado no pueden vivir en flash [C].** Escribir NVS o la partición del slot deshabilita el cache de flash. Un app XIP **no puede registrar ISRs propias**; el OS dueño de las interrupciones despacha por callbacks a tareas. Esto refuerza tu principio de "el OS es dueño del hardware" y lo hace obligatorio, no estético.
4. **Presupuesto de display: el framebuffer completo son 64.800 B (240×135×2) [C, cuenta directa].** Sin PSRAM es ~20 % del heap libre con WiFi activo. Hay que decidir entre framebuffer completo, renderizado por bandas o sprites, y es una decisión de la ABI de UI.
5. **Las 15 apps existentes son firmwares Arduino completos que inicializan el hardware ellas mismas [L].** No se "portan" recompilando: se reescriben contra servicios. Hay que priorizar. Ver §7.
6. **Conflicto de flash: slot de 4 MB + "boot to firmware" + coexistir con M5Launcher [L/C].** 8 MB − OS − 4 MB de slot deja poco para una imagen OTA de un firmware pesado. Propongo en §9 que el slot sea también la partición OTA (es cache evictable), a validar.
7. **Licencias de tus apps no son uniformes [L].** Tu repo de apps está bajo PolyForm Noncommercial 1.0.0 (no es open source OSI) y cuatro apps tienen licencia propia (GPL, CC BY-SA). Meterlas en el repo del OS exige licencia por directorio. Ver §10.

## 2. Tactility

### Qué verifiqué **[V]**
- "Un sistema operativo enfocado en la familia ESP32", carga apps desde SD, "construidas con SDKs nativos C/C++".
- Estructura del repo: `Tactility/`, `TactilityKernel/`, `TactilityFreeRtos/`, `Platforms/`, `Devices/`, `Drivers/`, `Modules/`, `Libraries/`, `Documentation/`, `Buildscripts/`.
- Tooling de apps: scripts `tt` / `tt.py` / `tt.ps1`, `tactility.py.json`, `device.py`.
- Tablas de particiones por tamaño de flash: `partitions-{4,8,16,32}mb-{with,no}-sd*.csv`.
- `lv_conf.h` en la raíz (sugiere LVGL; **no confirmado**).
- Licencia: `LICENSE.md` = **GPL-3.0**. La carpeta `Documentation/` incluye además `LICENSE-Apache-2.0.md` y `LICENSE-GPL-3.0.md`, lo que indica **licenciamiento mixto por componente**. No pude ver cuál parte es cuál.

### Qué **no** pude verificar
Cómo carga los ELF, cómo exporta símbolos, qué versionado de ABI usa, el modelo de servicios/ciclo de vida, y si tiene definición de la Cardputer ADV (el README no la menciona).

### Lo que me llevo (conceptual)
- Separación kernel / plataforma / devices / drivers: la definición de hardware es **datos por placa**, no `#ifdef`. Para nosotros hay una sola placa, así que no copio esa capa de configurabilidad (ver la regla de "no agregar flexibilidad especulativa" de tu `CLAUDE.md` del launcher).
- Un SDK + una CLI de build que empaqueta la app y la verifica contra la versión del SDK. Esto es lo que necesita nuestra Fase 5.
- Tablas de particiones prearmadas por tamaño: confirma que la tabla es parte del producto, no un detalle.

**Por la regla clean-room y por ser GPL-3.0 en el núcleo, no abro su código de carga ni de servicios.** Si más adelante necesito un detalle concreto, lo pregunto o lo derivo de la doc pública.

## 3. Flipper Zero (FAP)

### Verificado **[V]** (de `documentation/AppsOnSDCard.md` y `AppManifests.md`)
- **Formato**: un `.fap` es un **ELF** con metadatos y recursos empaquetados. El manifiesto es `application.fam` (build-time) y la versión de API que necesita queda grabada en el binario.
- **Tabla de API exportada**: el firmware expone solo un subconjunto de funciones/variables, listado en `api_symbols.csv` por target. El build genera la tabla de símbolos desde ese CSV y el cargador la usa para resolver imports. Si la app importa algo fuera de la tabla, el build advierte y la app no corre.
- **Versionado**: semver `major.minor`. **Major** sube por cambios incompatibles (borrar o cambiar la firma de algo), **minor** por agregados. El cargador exige que el major coincida. El doc no dice si compara el minor.
- **Control de cambios de ABI**: entradas nuevas del CSV aparecen como `?` y **el build del firmware no avanza** hasta que alguien las marca `+` (expuesta) o `-` (no disponible). Es un mecanismo de gobierno muy bueno: nadie expone API por accidente.
- **Carga**: la MCU no ejecuta desde almacenamiento externo, así que el cargador verifica integridad/compatibilidad, **reserva RAM, copia código y datos**, procesa relocaciones (asigna direcciones a los imports) y arranca. La app ocupa menos RAM que el tamaño del archivo (solo secciones de código y datos). Consecuencia que el doc nombra: correr desde RAM deja menos heap.
- **Manifiesto** (campos): `appid`, `apptype`, `name`, `entry_point`, `stack_size`, `requires`, `conflicts`, `order`, `sources`, `fap_category`, `fap_icon` (1 bit, 10×10), `fap_libs` (a costa de tamaño y RAM), `resources`, `sdk_headers`, `fap_version` (solo major.minor), `targets`, además de autor/descripción/weburl.
- Tipos de app: `SERVICE` (arranca temprano), `PLUGIN` (dentro del firmware), `EXTERNAL` (`.fap`), `METAPACKAGE`, entre otros.

### Lo que **no** cubre la doc que leí
- Los permisos de servicios: en Flipper no hay modelo de permisos, solo acceso a las APIs exportadas.
- "Records" y servicios de `furi` (`furi_record_open/close`): la doc de `FuriCheck`/`FuriHalBus` existe pero no la abrí. Mi conocimiento es **[C]**: un registro global nombrado donde un servicio publica una interfaz y las apps la abren por nombre con conteo de referencias; un `furi_record_open` bloquea hasta que el servicio la publica. Se parece a lo que necesito para el bus de servicios. Lo diseño por mi cuenta.

### Lo que me llevo
- **Tabla de API generada desde una lista curada con estados `+/-/?`**: la adopto como principio (en mi formato, no su CSV). Es la defensa contra exportar de más y romper la ABI sin darse cuenta.
- **Major/minor**: política concreta propuesta en la Fase 2: mismo major requerido; la app declara el *minor mínimo* que necesita y el OS rechaza si el suyo es menor (Flipper aparentemente no lo hace, pero para nosotros es barato y evita un crash tardío).
- **Lo que no copio**: Flipper no tiene suspensión ni back stack, y no tiene permisos. Son partes nuevas de nuestro diseño.

## 4. `espressif/elf_loader`

### Verificado **[V]** (resultados de búsqueda del registro, no el README completo)
- Es el componente `espressif/elf_loader` del registro; la última versión que vi es la **1.3.5**.
- API base: inicializar un handle `esp_elf_t`, **relocar** los bytes del ELF en él, luego un `request` que ejecuta la entrada, y `deinit`.
- El README de la 1.3.5 lista a **ESP32-S3 como soportado para ejecutar ELF desde PSRAM**. El changelog dice que la 1.3.4 corrigió un **deadlock en la relocación de ELF en PSRAM del S3 bajo carga multicore**.
- El README de la 1.3.4 listaba solo ESP32 como SoC soportado, lo que contradice a las otras versiones. **Hay que fijar una versión y probarla en nuestro S3.**
- Los ejemplos viven en el repo `esp-iot-solution` (ejemplos `elf_loader_example` y `elf_console_example`).
- **Licencia: no la encontré.** No la afirmo. Se verifica en la Fase 2 al fijar la versión.

### Implicaciones para nosotros
- El soporte documentado del S3 es **PSRAM**. Nosotros **no tenemos PSRAM**. Que funcione relocando a SRAM interna es una hipótesis a probar en la Fase 3, no un hecho.
- Supuestos **[C]** sobre cómo trabaja: espera ELF compilado como objeto compartido/PIC (`-fPIC -shared`), resuelve imports contra una tabla de símbolos que el host le provee, y asigna memoria con `MALLOC_CAP_EXEC` para el código. Esto último es justo el punto frágil de R1.
- Es **dependencia de build** vía `idf_component.yml`, sin vendorizar, tal como pediste. Si no sirve en SRAM sin PSRAM, el plan B es escribir un cargador propio. Un reubicador Xtensa para un ELF con pocos tipos de relocación (`R_XTENSA_RELATIVE`, `GLOB_DAT`, `JMP_SLOT`, `32`) **[C]** es chico (del orden de cientos de líneas) y evita depender de un componente que no está probado en nuestro caso. Lo decidimos con el spike.

## 5. ESP32-S3 sin PSRAM: memoria, MMU y cache

### 5.1 Verificado **[V]** (doc `mm.rst` de ESP-IDF, vía resumen)
- El driver `esp_mmap` mapea flash (y PSRAM donde existe).
- Capacidades de dirección virtual: `MMU_MEM_CAP_EXEC`, `READ`, `WRITE`, `32BIT`, `8BIT`.
- `esp_mmu_map` reserva un bloque virtual con las capacidades pedidas y lo mapea al bloque físico. Por defecto el mapeo es 1:1: un bloque ya incluido devuelve `ESP_ERR_INVALID_STATE` (con `out_ptr` apuntando al mapeo existente) y uno solapado `ESP_ERR_INVALID_ARG`. El flag `ESP_MMU_MMAP_FLAG_PADDR_SHARED` permite que una misma dirección física tenga varias virtuales.
- `esp_mmu_map_virt` mapea en una `vaddr_start` elegida (0 = cualquiera libre). `esp_mmu_unmap`, `esp_mmu_vaddr_to_paddr`, `esp_mmu_paddr_to_vaddr` completan la API.
- `esp_mmu_map_get_max_consecutive_free_block_size` informa el mayor bloque contiguo mapeable para unas capacidades dadas; la doc no da un máximo fijo.
- El acceso a flash por `esp_flash` o por punteros lo mantiene sincronizado el driver.
- **No cubierto por lo que leí**: tamaño de página, tamaños de cache, mapeo separado de instrucción y datos, ni restricciones de ejecutar código de una partición arbitraria.

### 5.2 Hechos del S3 que uso y **no verifiqué en esta sesión [C]**
- **SRAM**: ~512 KB internos. Una parte es SRAM0 (instrucciones y cache) y el resto SRAM1/SRAM2 accesible como datos por DBUS (`0x3FC8_8000…`) y, en SRAM0/SRAM1, también como instrucciones por IBUS (`0x4037_0000…`) a través de un **alias** de las mismas celdas físicas.
- **Cache**: parte de esa SRAM se usa como cache de instrucciones (16 o 32 KB) y de datos (32 o 64 KB), configurable en menuconfig. Eso baja el heap utilizable.
- **MMU**: páginas de 64 KB, la misma flash puede verse como instrucciones (IBUS, ~`0x4200_0000`) y como datos (DBUS, ~`0x3C00_0000`). Por lo tanto un slot XIP ocupa ventanas múltiplo de 64 KB: el desperdicio por app es hasta 64 KB.
- **Cache + escritura de flash**: mientras se borra/escribe flash, el cache se deshabilita; todo el código/datos de flash queda inaccesible para ambos núcleos hasta que termine, salvo código marcado `IRAM_ATTR`. La tarea que escribe debe estar "cache-safe". El impacto lo mido en la Fase 3.
- **Memory protection**: en IDF 5.x el S3 trae `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE` que impone permisos W^X por región. Ejecutar código copiado a DRAM por el alias IBUS puede **dispararlo**. Probablemente haya que desactivarlo (consistente con tu decisión "sin protección de memoria").
- **RTC memory**: 8 KB de RTC slow, con `RTC_NOINIT_ATTR` sobrevive a un reset por software o por panic (no a corte de energía). Sirve para el back stack tras crash, con CRC y número de secuencia. 8 KB alcanzan sobrado para un back stack de 4 niveles con punteros y ids, no para estado.
- **Crash info**: tras un panic, `esp_reset_reason() == ESP_RST_PANIC`; con la partición de coredump (64 KB) se puede leer PC/backtrace/`EXCVADDR` al volver (`esp_core_dump_get_summary`). Con la dirección base de la app (guardada en RTC) se traduce el PC a "app X + offset".

### 5.3 Dos formas de hacer el modo (b) (XIP)

El código XIP vive en flash y **no se puede modificar al cargar**. Eso fuerza:

| Opción | Cómo | Pros | Contras |
|---|---|---|---|
| **B1: ventana fija** | La app se linkea a una dirección virtual fija; el OS la mapea ahí con `esp_mmu_map_virt` | Sin relocaciones de código; solo se relocan los datos en RAM | Una ventana por app residente; el link time fija el layout; dos apps XIP a la vez exigen dos ventanas distintas |
| **B2: PIC** | `-fPIC`; las referencias van por una GOT en RAM que el OS completa | Cualquier dirección, N apps flexibles | Código más grande/lento; dependo de la calidad del soporte PIC de GCC para Xtensa y de que la GOT/PLT funcione con literales en `.text` |

La opción por defecto de mi propuesta es **B1 con ventanas fijas** (menos magia, menos riesgo), y se mide B2 en el spike solo si B1 no alcanza. Es una decisión de la Fase 2 que se confirma con datos en la Fase 3.

Dudas puntuales a resolver en el spike **[C]**:
- ¿Se pueden leer `.rodata` y literales desde la ventana de instrucciones, o hay que mapear también la ventana de datos del mismo bloque físico?
- ¿Cuánto cuesta el cache miss en un app XIP con cache de 16 KB (vs correr desde IRAM)?
- ¿`esp_mmu_map_virt` está disponible y es estable en la versión de IDF que fijemos?

## 6. Android (referencia conceptual)

**Adopto**
- Ciclo de vida explícito con guardado de estado separado de la destrucción: `onSaveInstanceState` → proceso muerto → `onCreate(savedInstanceState)`. Mapea 1:1 a tu `on_save_state / on_restore_state`.
- **Intent**: acción + tipo de dato + payload; resolución por *intent filters* declarados en el manifest; `startActivityForResult` para respuesta. Mapea a tus intents con resultado.
- **Task/back stack** gestionado por el sistema, con vuelta a la pantalla anterior reconstruida desde su estado guardado.
- **`onTrimMemory`**: aviso graduado de presión de memoria. Lo adopto como evento del OS hacia las apps ("liberá caches") antes de recurrir a la descarga.
- **Límite de Bundle**: Android advierte que el estado guardado debe ser chico. Tu tope de 4 KB por intent y la arena para estado son la misma idea.

**No adopto**: procesos aislados, Binder, servicios en background persistentes con prioridades, broadcasts globales. Sin protección de memoria y con ~200 KB de heap no aplican. El bus pub/sub queda **previsto, no implementado**.

**Decisión de suspensión (tu requisito)**: "pausar sin descargar si la RAM lo permite; si no, serializar a la arena y descargar" equivale al modelo de Android de *proceso cacheado vs. proceso muerto con estado guardado*. La política (umbral de heap libre con histéresis) se define en la Fase 2 y se calibra en la Fase 3.

## 7. Inventario de tus apps y qué piden del OS **[L]**

Tomado de `Cardputer-ADV-Apps/apps/*` (líneas de C/C++ aproximadas; todas hoy usan `M5Cardputer` sobre Arduino y se instalan como `.bin` con M5Launcher).

| App | LOC | Servicios que necesita | Modo sugerido | Notas |
|---|---|---|---|---|
| snake | 318 | display, teclado, audio out, storage chico (high score) | ELF-RAM | Candidata a app de ejemplo n.º 1 |
| tetrix | 648 | ídem | ELF-RAM | |
| tuner | 590 | display, teclado, **micrófono (audio in)**, FFT | ELF-RAM | Audio in compartido/exclusivo a definir |
| compass | 572 | display, teclado, **IMU** | ELF-RAM | IMU compartido |
| led-rc | 396 | display, teclado, **IR TX** | ELF-RAM | IR TX exclusivo con lease |
| tv-b-gone | 9.136 | display, teclado, **IR TX** | XIP | El tamaño sale de las tablas de códigos; son datos, ideales para XIP. Licencia propia (CC BY-SA 2.5) |
| wiz-rc | 859 | display, teclado, **WiFi UDP** | ELF-RAM | Caso natural para WiFi compartido |
| ekoparty-badge | 1.507 | display, teclado | XIP | Assets (logo) pesados en flash |
| karting | 1.801 | display, teclado, **IMU, SD** | XIP | Logging a SD |
| shell-rc | 646 | display, teclado, **BLE central** | ELF-RAM | NimBLE hoy; en el OS pasa a servicio BLE |
| ble-sentinel | 2.814 | display, teclado, **BLE scan**, audio out | XIP | Scan-only |
| mp3-player | 2.708 | display, teclado, **SD, audio out**, decoder | XIP | Prueba de fuego de la Fase 5. Usa `ESP8266Audio`; licencia propia en su carpeta |
| nrf24-jammer | 368 | **bus EXT/SPI exclusivo** | pendiente | App de RF ofensiva: conviene una revisión de política/legal antes de incluirla en un catálogo público. No la considero para el MVP |
| doom | n/d | display, teclado, audio, SD (WAD) | **boot-to-firmware** | GPL-2.0-or-later; mucha RAM |
| meshtastic-ra02 | 106 (+ upstream) | todo el hardware | **boot-to-firmware** | Es un firmware completo; tal como indicaste |

Observaciones:
- **Lo que piden suma al menos 9 servicios**: display, teclado, SD, audio out, audio in, IMU, IR TX, WiFi, BLE, y bus EXT. El MVP de la Fase 4 (display, teclado, SD, WiFi) cubre bien a `snake`, `tetrix`, `wiz-rc`, y el caso de prueba (escaneo WiFi → detalle).
- Sin servicio de audio no hay `mp3-player` ni `tuner`; la prueba de fuego de la Fase 5 arrastra audio out y SD de alta tasa.
- El mayor volumen de LOC está en `tv-b-gone` (datos) y `ble-sentinel`/`mp3-player`/`karting`: ahí XIP rinde.
- Todas, salvo `doom` y `meshtastic-ra02`, dependen de `M5Cardputer` (Arduino). El port **no es recompilar**: es reemplazar `M5Cardputer.Display/Keyboard/Speaker/...` por llamadas a la ABI. La API del OS debería ser lo bastante parecida en forma para que el port sea mecánico, sin copiar nada de M5.

## 8. Cómo instala hoy tu Launcher **[L]**

`CardputerADV-Launcher/src/appStore.cpp` implementa el menú APPS:
- Repo por defecto `AMasetti/Cardputer-ADV-Apps`, directorio de SD `/apps/`.
- Lista las **tags** de GitHub por la API (`/repos/<repo>/tags`), hasta 5 páginas de 100, con parseo en streaming para no retener el cuerpo en RAM.
- Convención de tag **`<app>-vMAJOR.MINOR.PATCH`**; se queda con la versión más alta por app; ignora `sd-latest` y pre-releases.
- Descarga el asset `releases/download/<tag>/<tag>.bin` a la SD y deja solo la última versión instalada.
- Maneja `404` (repo privado), `403` (rate limit) con mensajes de error claros.

**Lo que adopto para el OS** (lo que pediste: "instalar vía referencia al repo, como el launcher"):
- Mismo esquema de **tags por app + asset por release** y un repo de referencia configurable. Los artefactos de CI de la Fase 5 siguen esta convención, así tu Launcher y el OS pueden listar el mismo catálogo.
- El asset pasa de `.bin` (firmware completo) a un contenedor de app (§Fase 2: ELF + manifest). Para no romper tu Launcher actual, los dos formatos pueden convivir en el mismo repo con sufijos distintos.
- **Dónde viven las apps**: según tu mensaje, el catálogo se hostea **en `CardputerOS`** de ahora en más. Eso implica monorepo `os/ + sdk/ + apps/` (propuesto en la Fase 2) y que el workflow de CI publique por app con el esquema de tags de arriba.

Otras piezas del Launcher relevantes para el boot-to-firmware **[L, por nombres de archivo; no las leí enteras]**: `partitioner`, `partition_table_model`, `partition_install_layout`, `app_registry`. Existe un repartidor de tabla en runtime. Ver riesgo R5.

## 9. Flash: restricciones para la tabla de particiones

Datos: flash de 8 MB; el Launcher usa una partición `app0` de `0x150000` (1,31 MB) y tiene el 99,5 % ocupado **[L]**, y los apps actuales usan `default_8MB.csv` **[L]**.

Tensión: OS (≈1,3–2 MB) + slot (≈4 MB) + NVS/otadata/coredump (≈0,2 MB) deja **≈1,5–2,5 MB** para una imagen OTA de firmware pesado. Meshtastic para S3 probablemente supere eso **[C]**.

Propuesta a validar en la Fase 2: **el slot de apps es una partición `app` (tipo OTA) de ≈4 MB, usada de dos maneras**:
1. Cache de apps XIP, subdividida por el OS con su propia contabilidad (tamaños múltiplos de 64 KB).
2. Para "boot to firmware", el OS **desaloja** el cache y escribe ahí el firmware pesado, lo selecciona con `otadata` y reinicia. El cache es reconstruible desde la SD, así que perderlo solo cuesta tiempo.

Pregunta abierta para vos (no la asumo): **¿cómo vuelve al OS un firmware pesado?** M5Launcher resuelve alguna variante de esto y no pude inferirlo de lo que leí. Y **¿conviven en la misma tabla de particiones con tu M5Launcher o es una u otra instalación?** El repartidor del Launcher reescribe la tabla, así que "conviven" necesita un contrato explícito.

## 10. Licencias

### Del repo
Propongo **Apache-2.0** para el OS y el SDK:
- Concesión explícita de patentes y compatibilidad con ESP-IDF (Apache-2.0 **[C]**), LVGL y FreeRTOS (MIT **[C]**).
- Las apps terceras se enlazan contra la tabla de símbolos del OS; con GPL/LGPL en el núcleo esa frontera sería discutible. Con Apache-2.0 no hay ambigüedad para apps cerradas o con otra licencia.
- Es el camino opuesto al de Tactility (GPL-3.0 en el núcleo **[V]**), lo cual además refuerza el límite clean-room.

### Dependencias candidatas, por verificar al fijar versiones en la Fase 2
| Dependencia | Licencia esperada | Estado |
|---|---|---|
| ESP-IDF | Apache-2.0 | [C] |
| `espressif/elf_loader` | no encontrada | **[V] no la hallé**; verificar |
| LVGL (si se usa) | MIT | [C] |
| FreeRTOS (incluido en IDF) | MIT | [C] |
| NimBLE (en IDF) | Apache-2.0 | [C] |
| TinyCBOR (si se usa) | MIT | [C] |

### Las apps
Tu `Cardputer-ADV-Apps` declara **PolyForm Noncommercial 1.0.0** salvo cuatro carpetas con licencia propia (`doom`, `mp3-player`, `meshtastic-ra02`, `tv-b-gone`; `doom` es GPL-2.0-or-later y `tv-b-gone` CC BY-SA 2.5) **[L]**. Consecuencias:
- Como autor podés relicenciar tus propias apps, pero **PolyForm NC no es una licencia de código abierto OSI**. Hay que decidir si el catálogo del OS las publica así o con otra licencia.
- La licencia debe ir **por directorio de app** (un `LICENSE` en cada una y un campo `license` SPDX en el manifest), y el instalador debería mostrarla.
- Los logos de terceros (Ekoparty) quedan fuera de cualquier licencia nuestra **[L]**.

## 11. Riesgos

Probabilidad (P) e impacto (I): A/M/B.

| # | Riesgo | P | I | Mitigación / cómo se mide |
|---|---|---|---|---|
| R1 | No se puede ejecutar código copiado a SRAM interna (memprot/PMS, heap sin EXEC suficiente) | M | A | Spike: cargar un blob a SRAM vía alias IBUS con memprot off; reservar arena de código en el linker. Fallback: solo XIP + apps mínimas |
| R2 | PIC en Xtensa no funciona bien o B1 (ventana fija) no alcanza para varias apps | M | A | Spike B1 primero, luego B2; medir tamaño y velocidad |
| R3 | Heap libre con WiFi on insuficiente para app + arena de estado (sin PSRAM) | A | A | Medir en el spike; presupuesto explícito (Fase 2); el WiFi/BLE se enciende bajo demanda |
| R4 | Escritura de flash (NVS, slot) congela el cache y pausa a las apps XIP | M | M | Escrituras solo en eventos críticos (ya está en tu diseño); batear escrituras; medir la pausa |
| R5 | Coexistir con M5Launcher: sus particiones/repartidor pueden pisar el slot | M | A | Definir contrato de tabla de particiones en la Fase 2; confirmar con vos |
| R6 | `elf_loader` no soporta S3 sin PSRAM o tiene bugs (hubo un deadlock en S3 PSRAM) | M | M | Fijar versión; plan B: reubicador propio mínimo |
| R7 | Crash de una app deja hardware en estado inconsistente (leases, SPI a media transacción, IR TX pegado) | A | M | Reset completo tras crash (ya en tu diseño); inicializar todo en el arranque; mostrar info del crash |
| R8 | ABI mal gobernada: exportar de más y no poder retirar | M | A | Lista curada con estados `+/-/?` (Flipper); CI que falla si cambia la tabla sin subir versión |
| R9 | UI: LVGL consume demasiado heap con 240×135 sin PSRAM, o el framebuffer completo no entra | M | M | Medir LVGL vs. toolkit propio mínimo; render por bandas con buffers DMA de ~10 KB |
| R10 | Las apps existentes (Arduino/M5Cardputer) se reescriben, no se portan | A | M | Priorizar 3 apps; hacer la API de servicios similar en forma a la que ya usan |
| R11 | Licencia mixta del catálogo (PolyForm NC, GPL, CC BY-SA) | A | M | Licencia por app + campo SPDX; revisión antes de publicar |
| R12 | Doc de ESP-IDF no leída por red bloqueada: supuestos [C] erróneos | M | M | Habilitar dominios o validar todo en Fase 3 |
| R13 | Concurrencia: dos núcleos, sin protección de memoria, apps y servicios compartiendo estado | M | A | Modelo: servicios en tareas del OS, apps hablan por colas/mensajes; sin punteros compartidos mutables |

## 12. Decisiones validadas / a ajustar

| Decisión tuya | Veredicto | Comentario |
|---|---|---|
| Dos modos de carga (RAM y XIP) | **Se sostiene, con reservas** | XIP es el modo principal; ELF-RAM depende de R1 y quizá quede limitado a apps de decenas de KB |
| Slot ≈4 MB con cache de apps recientes | **Se sostiene** | Mejor si también es la partición OTA del boot-to-firmware (§9) |
| ABI por tabla de símbolos versionada | **Se sostiene** | Agregar estados `+/-/?` y minor mínimo declarado |
| Manifest por app | **Se sostiene** | Agregar `license` (SPDX), `stack_size`, `heap_hint` y permisos |
| Apps pesadas por boot-to-firmware | **Se sostiene** | Falta definir el retorno al OS y la convivencia con M5Launcher |
| El OS dueño de todo el hardware | **Se sostiene y es necesario** | Las ISRs en flash no existen (§1.3) |
| Lease exclusivo vs. compartido | **Se sostiene** | La lista de servicios sale de §7 |
| Intents ≤ 4 KB CBOR/TLV | **Se sostiene** | CBOR vs. TLV propio: decidir en la Fase 2 por tamaño de código |
| Back stack default 4 | **Se sostiene** | RTC de 8 KB alcanza para la metadata |
| Suspensión automática por heap libre | **Se sostiene** | Umbral con histéresis; se calibra con datos |
| Persistencia solo en eventos críticos, atómica | **Se sostiene** | Con SD por SPI y batería, definir "batería baja" como evento |
| Sin protección de memoria | **Se sostiene** | Y probablemente obligue a apagar `memprot` (R1) |
| Sin Arduino en el core | **Se sostiene** | Cuidado: las apps actuales dependen de Arduino |

## 13. Fuentes

- Tactility (README, estructura del repo, `Documentation/`): <https://github.com/TactilityProject/Tactility>, <https://raw.githubusercontent.com/TactilityProject/Tactility/main/README.md>, <https://github.com/TactilityProject/Tactility/tree/main/Documentation>
- Flipper Zero — FAP y manifiestos: <https://raw.githubusercontent.com/flipperdevices/flipperzero-firmware/dev/documentation/AppsOnSDCard.md>, <https://raw.githubusercontent.com/flipperdevices/flipperzero-firmware/dev/documentation/AppManifests.md>, <https://github.com/flipperdevices/flipperzero-firmware/tree/dev/documentation>
- ESP-IDF, mapeo de memoria: <https://raw.githubusercontent.com/espressif/esp-idf/master/docs/en/api-reference/system/mm.rst>
- `espressif/elf_loader` (solo vía resultados de búsqueda; la página del registro está bloqueada): <https://components.espressif.com/components/espressif/elf_loader/>, <https://components.espressif.com/components/espressif/elf_loader/versions/1.3.5/readme>, <https://components.espressif.com/components/espressif/elf_loader/versions/1.3.5/changelog>
- Tus repos (leídos localmente): `Cardputer-ADV-Apps` (`README.md`, `LICENSE`, `apps/*`, `docs/hardware.md`, `.github/workflows/build.yml`) y `CardputerADV-Launcher` (`src/appStore.cpp`, `src/app_registry.h`, `boards/m5stack-cardputer/CardputerADV.md`).

## 14. Pedidos antes de pasar a la Fase 2

1. **OK o ajustes** a este documento.
2. **Red**: ¿habilitás `docs.espressif.com`, `components.espressif.com` y `developer.flipper.net`? Cierran las marcas **[C]** más importantes (R1, R2) antes de diseñar.
3. **Licencia**: ¿Apache-2.0 para OS y SDK? Y para las apps en el catálogo, ¿se mantiene PolyForm NC o querés relicenciar alguna?
4. **Boot-to-firmware**: ¿cómo vuelve hoy un firmware pesado a tu M5Launcher, y el OS reemplaza a M5Launcher o conviven en la misma tabla de particiones?
5. **Hardware**: ¿vas a medir en tu placa en la Fase 3 y pegarme el log serie?
