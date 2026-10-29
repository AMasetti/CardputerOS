# Changelog — Shell RC

## 0.1.3
- Intents (CardputerOS 0.7.0 and later, docs/intents.md): car.drive, car.turbo, car.donut, car.battery.

## 0.1.2
- New vector launcher icon (CardputerOS 0.6.0 and later; 0.5.0 icons were bitmaps).

## 0.1.1
- Launcher icon (shown by CardputerOS 0.5.0 and later).

## [0.1.0]
- Elegís la generación (2024 `QCAR-*` o 2025 `SL-*`) y busca los autos por BLE, ordenados por señal.
- 2024: comandos de 16 bytes cifrados con AES-128 ECB y 4 velocidades (`1`-`4`).
- 2025: comandos de 8 bytes con luces (`l`), turbo (`t`), donut (`o`), modo 1/2 (`m`) y batería en pantalla.
- Manejo con `;` `.` `,` `/` (o WASD) mantenidas, envío continuo cada 30 ms y parada al desconectar (`del`).
- Recuerda la última generación elegida.
