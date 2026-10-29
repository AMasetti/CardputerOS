# Robot Control

Un solo control remoto para robots y drones de juguete (interior). La pantalla de inicio busca por
Bluetooth LE y por WiFi a la vez y lista lo que encuentra; enter conecta, **Fn + `** sale.

| Robot | Radio | Se reconoce por | Extras |
|---|---|---|---|
| BB-8 original, Ollie (Sphero v1) | BLE | `BB-…`, `2B-…` | color, calibrar, dormir |
| R2-D2, R2-Q5, BB-9E, BOLT (Sphero v2) | BLE | `D2-…`, `Q5-…`, `GB-…`, `SB-…` | color, calibrar, cabeza y patas (R2), sonido |
| Parrot Rolling Spider | BLE | `RS_…` | despegar/aterrizar, flips, ruedas, emergencia |
| Parrot Jumping Sumo | WiFi | red `JumpingSumo_…` | video, saltos, animaciones, posturas |

## Teclas comunes
`;` `.` adelante/atrás (mantenidas) · `,` `/` girar · `-` `=` velocidad · `space` frena · `backspace` vuelve a la lista.
Soltar las teclas de manejo frena al robot. Cada driver muestra sus teclas propias en pantalla.

* **Sphero (v1 y v2):** `1`-`8` color; `c` calibra (gira con `,` `/` hasta que el frente apunte donde quieras y enter); `s` duerme.
  R2: `h` `g` cabeza, `w` `t` `y` patas, `p` sonido.
* **Rolling Spider:** `t` despega (hace el flat trim antes), `l` aterriza, `x` emergencia, `w` `s` altura, `a` `d` lateral, `1`-`4` flips, `v` ruedas.
* **Jumping Sumo:** `j` `l` saltos, `1`-`9` animaciones, `u` `k` `y` posturas, `v` video.

## Cómo está hecho
Un archivo de protocolo por familia, sin dependencias del OS (se prueban en el host), y un driver por familia que implementa `Robot` (`src/robot.h`):

* `sphero1_core.h` — paquetes `FF SOP2 DID CID SEQ DLEN DATA CHK`, UUID `22bb746f-2bXX-…`
* `sphero2_core.h` — paquetes `8D … D8` con escape y checksum, UUID `00010002-574f-4f20-5370-6865726f2121`
* `minidrone_core.h` — comandos ARCommands por las características `FA0A`/`FA0B`/`FA0C` y notificaciones `FB0E`/`FB0F`
* `sumo_core.h` — tramas ARNetwork por UDP, comandos, reensamblado del video MJPEG

Tests: `g++ -std=c++17 apps/robot-control/test/<x>_test.cpp -o /tmp/t && /tmp/t` (sphero1, sphero2, minidrone) y
`/usr/bin/python3 apps/robot-control/test/run_sumo_test.py` (Sumo simulado, con Pillow).

## Estado
Armado con documentación pública de los protocolos; probado contra simulaciones y en el emulador, **sin probar con robots reales**.
A verificar en el hardware: Sphero v1 (potencia TX `0x07`, luz trasera `0x21`, fijar rumbo `0x01`); Sphero v2 (si BB-9E/R2 aceptan comandos
sin destino, máscaras de LED por modelo, BOLT con destino `0x12`, un solo sonido conocido); Rolling Spider (orden del handshake, qué
notificaciones hacen falta); Sumo (tamaño de fragmentos de video). Sphero Mini y RVR no están soportados.
