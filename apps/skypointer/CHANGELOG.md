# Changelog — Buscador de cielo

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
