# Changelog — MP3 Player

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
