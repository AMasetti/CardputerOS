# Changelog — Sumo RC

## [0.1.0]
- Se conecta al WiFi del Jumping Sumo, hace el handshake y maneja con el teclado.
- Video MJPEG de la cámara a 160x120 (decodificado por el OS con `media.jpeg_draw`).
- Saltos, animaciones y posturas; batería y alertas del robot.
- Modo demo sin robot para probar la pantalla.
- El protocolo vive en `sumo_core.h` y se prueba en el host contra un Sumo simulado
  (`python3 apps/sumo-rc/test/run_host_test.py`). Sin probar todavía con el robot real.
