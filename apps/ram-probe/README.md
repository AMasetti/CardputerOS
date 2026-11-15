# ram-probe

App mínima para aislar el problema de carga de apps (BRIEF-v2): `perms: []`, stack 4096, sin WiFi/SD/IMU/GNSS/audio/BLE ni librerías externas, sin canvas propio (dibuja directo en la pantalla).

Muestra el título, "cargada: OK" y el **heap libre** (`cpos_api->free_heap()`, la única medición de memoria que ofrece la ABI a las apps; no hay bloque más grande ni memoria ejecutable). Salir: **Fn + `**.

## Qué mide y qué no
- Que cargue o no con el mismo loader que Skypointer, y su tamaño de ELF (segmentos de código/datos con `xtensa-esp32s3-elf-readelf -lW`).
- Los números reales (heap libre, bloque más grande, memoria ejecutable libre) **no** salen en pantalla: se obtienen del `ESP_LOGI` "start ..." del runner y del error numerado del loader ("code no room: need ..B, ... free ...B, max ...B").
