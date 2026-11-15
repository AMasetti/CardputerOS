# Buscador de cielo (skypointer)

Sostenés el Cardputer-Adv como una cámara y la pantalla es una ventana al cielo: sobre lo que estás
apuntando se dibujan estrellas, constelaciones, el Sol, la ISS y el tráfico aéreo, en su posición real.
La posición se calcula con el GPS, la hora UTC y la actitud del equipo (el BMI270).

## Cómo se usa

1. Con el Cap LoRa-1262 espera el fix del GPS (también da la hora). Sin GPS, la hora se pone con `s`
   (NTP por WiFi) y la posición es la última que se guardó con fix. Si no hay hora o no hay posición,
   no dibuja nada y lo dice en pantalla.
2. Dejalo quieto sobre la mesa mientras calibra el giróscopo.
3. Fijá el rumbo (el BMI270 tiene 6 ejes: no hay magnetómetro, el norte no se conoce solo):
   - `n`: apuntá el borde de arriba al norte (o, si lo tenés parado, la parte de atrás) y apretá. Queda
     con un error de unos 3°.
   - `enter`: apuntá el equipo a la referencia que propone la app (el Sol si está a más de 10°; si no, la
     estrella más brillante sobre 20°, marcada con un círculo verde, o con una flecha en el borde si
     está fuera de pantalla) y apretá. Es lo más preciso: se mide contra la altura real.
   - `,` y `.`: mueven el cielo 1° a la izquierda o a la derecha (si las dejás apretadas se repiten).
4. El rumbo deriva de a poco con el giróscopo: abajo a la derecha dice el error estimado (`rumbo ±N°`,
   verde, amarillo o rojo). Cuando se pone feo, volvé a fijarlo. La inclinación no deriva.

## Teclas

| Tecla | Qué hace |
|-------|----------|
| `tab` | Capa: Estrellas · Satélites · Aéreos · Todo |
| `n` | Fijar el norte con el borde de arriba |
| `enter` | Fijar el rumbo apuntando a la referencia propuesta |
| `,` `.` | Mover el cielo 1° a la izquierda / derecha |
| `s` | Sincronizar por WiFi: hora (NTP) y TLE de la ISS |
| `c` | Recalibrar el giróscopo (dejalo quieto) |
| `x` | Cambiar la orientación del sensor si los ejes no coinciden (se guarda) |

Las capas eligen qué objetos se dibujan; el horizonte (0°), las marcas cada 30° de azimut, la cruz
central, el Sol y el HUD (azimut y altura de adonde apuntás, hora UTC, satélites GPS, capa y rumbo)
están siempre. Las estrellas bajo el horizonte se ven apagadas.

## De dónde salen los datos

- **Estrellas y constelaciones**: `src/catalog.h`, generado por `tools/make_catalog.py` con HYG
  Database v4.1 (astronexus, CC BY-SA 4.0) y las líneas de d3-celestial (ofrohn, BSD). 2093 estrellas
  y 743 segmentos.
- **Sol y paso a horizonte**: `src/astro.cpp` (Meeus abreviado, precesión IAU 1976, refracción de
  Bennett). **Satélites**: SGP4 de Vallado portado a C por aholinch (dominio público), en
  `src/vendor`. Todo se verifica contra PyEphem con `python3 tools/verify_astro.py` (hace falta
  `pip install ephem`).
- **Posición y hora**: GNSS del Cap LoRa-1262 (el mismo lector NMEA que las apps de Meshtastic), o la
  última posición guardada en la flash y la hora por NTP.
- **TLE de la ISS**: `http://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE` (HTTP plano:
  la app no tiene TLS). Se guarda en `/sdcard/skypointer/tle.txt` de la SD y en la flash, y solo se vuelve a
  bajar si tiene más de 7 días. La red es la que guardaste en System > WiFi del OS (permiso `wifi`).
  El archivo puede tener hasta 3 TLE (nombre + 2 líneas cada uno) para seguir otros satélites.
- **Tráfico aéreo**: `/sdcard/skypointer/aircraft.json` en la SD, que se lee al entrar a las capas Aéreos o
  Todo. Es una lista como `[{"flight":"JES3176","lat":-32.94,"lon":-60.66,"alt":34000,"track":61.2}]`
  (coordenadas decimales, altitud en pies, rumbo en grados) o el sobre `{"ac":[...]}` de
  adsb.lol / adsb.fi, así que sirve pegar la respuesta cruda. Cada avión es un triángulo orientado por
  su rumbo con el vuelo y la altitud en pies. Se toman hasta 64 (los más cercanos). La posición del
  archivo no se extrapola: es una foto.

## Qué falta

- Aéreos en vivo: los feeds ADS-B (adsb.lol, adsb.fi, OpenSky) redirigen a HTTPS y la app no tiene
  TLS, así que hay que bajar el archivo desde otro lado (la PC, el teléfono) y copiarlo a la SD.
- Luna y planetas (falta un modelo verificado; queda para 0.2.0).
- Posición manual: sin GPS hace falta haber tenido un fix alguna vez.
- Rumbo absoluto sin magnetómetro: hay que fijarlo a mano y se corre con el tiempo.

## Licencias

- SGP4: dominio público (`third_party/LICENSE-sgp4.txt`).
- HYG Database: CC BY-SA 4.0 (`third_party/LICENSE-hyg.txt`); el catálogo derivado conserva esa licencia.
- d3-celestial: BSD (`third_party/LICENSE-d3celestial.txt`).
