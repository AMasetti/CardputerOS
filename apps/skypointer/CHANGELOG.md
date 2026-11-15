# Changelog — Buscador de cielo

## [0.1.2]
- Arreglo del techo de RAM ejecutable: con el catálogo en el segmento de código la app llegaba a
  83,6 KB de `.text` y el loader cortaba con "not enough executable RAM" (0.1.0 había cargado con
  63,9 KB). El catálogo vuelve a `.rodata` y el código queda en 63,9 KB, el tamaño que ya se sabe
  que entra.
- El catálogo queda en 1355 estrellas (mag <= 4,2 más todas las que tienen nombre propio o Bayer,
  que son las que dibujan las constelaciones) y 681 segmentos. El bloque de datos queda en 18 KB.
- Aéreos: hasta 20 aviones en vez de 32.

## [0.1.1]
- Arreglo de RAM: el OS no podía reservar el segmento de datos de la app (38 KB en un solo bloque
  contiguo) y el launcher cortaba con "not enough RAM for app data". El catálogo de estrellas y las
  líneas de constelaciones ahora viven en el segmento de código, que el OS copia en pedazos: el
  bloque de datos bajó a 5,4 KB.
- El catálogo quedó más compacto sin perder estrellas: 6 bytes por estrella (sin el índice de color,
  que se reemplazó por un blanco tibio) y las líneas de constelaciones como pares de índices a la
  estrella más cercana (4 bytes por segmento en vez de 12).
- Los satélites se piden al heap cuando hay TLE (el ElsetRec del SGP4 pesa 1,6 KB cada uno) y el
  tráfico aéreo se guarda hasta 32 aviones.

## [0.1.0]
- Primera versión. Sostenés el Cardputer como una cámara (campo de 55°) y la pantalla muestra, en su
  posición real de azimut y altura: estrellas (2093, hasta mag ~6) con nombre las más brillantes, líneas
  de constelaciones, el Sol (con refracción), la ISS con su traza de 10 minutos y el tráfico aéreo.
- Capas con tab: Estrellas, Satélites, Aéreos, Todo. Horizonte, marcas cada 30° y cruz central siempre.
- Posición del GPS del Cap LoRa-1262 (se guarda en la flash para cuando no hay fix) y hora UTC del GPS o,
  con s, por NTP.
- Actitud con el filtro de Mahony de la brújula. El rumbo se fija con n (borde de arriba al norte) o con
  enter (apuntando al Sol o a la estrella más brillante sobre 20° que propone la app); , y . lo corrigen
  de a 1°. Muestra el error de rumbo estimado.
- TLE de la ISS de Celestrak por HTTP (s), guardado en /sdcard/skypointer/tle.txt y en la flash; se vuelve a
  bajar solo si tiene más de 7 días.
- Aéreos desde /sdcard/skypointer/aircraft.json de la SD (lista o sobre {"ac":[...]} de adsb.lol / adsb.fi).
- Necesita CardputerOS 0.8.0 o posterior (GNSS, DNS); permisos: IMU, GNSS, WiFi y tarjeta SD.
