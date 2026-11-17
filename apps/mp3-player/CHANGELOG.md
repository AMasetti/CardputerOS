# Changelog — MP3 Player

## 0.2.6
- No reinicia el sistema al abrir con música en la SD. Con la biblioteca por fin visible (0.2.5), la RAM no
  alcanzaba: canvas de pantalla completa (65 KB) + 48 KB reservados de entrada para los textos. Ahora la
  pantalla se dibuja en tres franjas de 240x45 (22 KB) y la reserva de textos depende de cuántos temas hay.
- Si aun así falta memoria (bibliotecas muy grandes), la app se cierra y el launcher dice "not enough
  memory" en lugar de reiniciar todo (SDK: `new` sin memoria termina la app).

## 0.2.5
- Vuelve a encontrar la música: las carpetas se abrían como `/sd/...` (la ruta del firmware Arduino) y
  CardputerOS sólo deja abrir `/sdcard/...`, así que no se veía nada dentro de `/music`. Lo mismo
  afectaba a las tapas `cover.jpg`/`folder.jpg`, al borrado de la caché de tapas y a la lista de playlists.
- Con CardputerOS 0.9.3 o posterior, las carpetas y archivos con tildes o ñ se abren bien.

## 0.2.4
- Intents (CardputerOS 0.7.0 and later, docs/intents.md): music.play_playlist, music.shuffle_all, music.playlists, music.volume, music.shuffle.

## 0.2.3
- New vector launcher icon (CardputerOS 0.6.0 and later; 0.5.0 icons were bitmaps).

## 0.2.2
- Launcher icon (shown by CardputerOS 0.5.0 and later).

## [0.2.1]
- Versión para CardputerOS 0.3.0: el decodificador MP3 y las tapas JPEG corren en el
  sistema, y la app se puede cargar en partes cuando la memoria está fragmentada.
- Las tapas en PNG no se muestran (solo JPEG).

## [0.2.0]
- Menús estilo iPod: Playlists, Artists, Albums, Songs, Shuffle Songs y Settings.
- Biblioteca armada con los tags ID3 (v2.2/2.3/2.4, Latin-1/UTF-8/UTF-16), con
  respaldo en los nombres de carpeta y archivo. Se indexa en
  `/music/.mp3player/library.tsv` y en cada arranque solo se leen los archivos nuevos.
- Pantalla Now Playing con tapa del disco (cover/folder.jpg o la imagen embebida,
  JPEG o PNG), título, artista, disco, año, barra de avance y tiempo transcurrido/restante.
- Mantener `,` `/` retrocede o adelanta dentro del tema.
- Playlists `.m3u` en `/music/Playlists`: crear, renombrar, borrar, agregar temas
  (uno, un disco o un artista entero), quitar y reordenar.
- Shuffle, repeat (off/all/one) y volumen se recuerdan entre usos.
- El audio se decodifica en una tarea propia en el núcleo 0, así la interfaz no corta el sonido.
- Fuente con acentos, ñ y otros caracteres no ASCII.

## [0.1.0]
- Lista de temas de `/music` (hasta 2 niveles de subcarpetas), ordenada.
- Play, pausa, anterior/siguiente, volumen y shuffle desde el teclado.
- Título y artista desde los tags ID3.
- Salida por `M5.Speaker` (parlante en la Cardputer, ES8311/jack en la Adv).
