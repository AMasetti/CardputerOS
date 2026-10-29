# Sumo RC

Control remoto del **Parrot Jumping Sumo** desde la Cardputer ADV, con video de la cámara.

## Uso
1. Prendé el Sumo (crea la red `JumpingSumo_xxxxxx`, sin clave).
2. Abrí la app: lista las redes Sumo cercanas. Enter conecta.
3. Manejo con las flechas del teclado (`;` `.` `,` `/`), mantenidas. Soltar frena.

`-` / `=` velocidad, `j` / `l` salto alto / largo, `1`-`9` animaciones, `u` `k` `y` posturas,
`space` corta la animación, `v` video on/off. `d` en la primera pantalla abre un modo demo sin robot.
**Fn + `** sale.

Es un robot de interior: usalo sin escaleras ni mascotas cerca y con el robot a la vista.

## Cómo funciona
* El OS une la placa al WiFi del Sumo; el handshake va por TCP a `192.168.2.1:44444` y el resto por UDP.
* `src/sumo_core.h` es el protocolo (tramas ARNetwork, comandos ARCommands, reensamblado del video
  y sus ACK). No usa sockets ni el OS, así que se prueba en el host:
  `python3 apps/sumo-rc/test/run_host_test.py` levanta un Sumo simulado y comprueba handshake,
  comandos, eventos, ping/pong y 40+ cuadros de video reensamblados byte a byte.
* El video es MJPEG 640x480; el OS lo decodifica con el decodificador de la ROM del ESP32-S3 a 1/4
  (160x120), saltando cuadros si el manejo no puede esperar.

## Estado
Probado contra un robot simulado y en el emulador. **Sin probar todavía con un Sumo real.** Lo que más
puede variar con el firmware real: tamaño de fragmentos de video, el orden de los fragmentos y si hace
falta algún comando extra para arrancar el video.
