# Changelog — Ekoparty 22 Badge

## 0.1.4
- Vuelve a abrir en CardputerOS 0.7.0 y posteriores. El logo RGB565 ocupaba 33,8 KB del bloque de
  datos, que el OS tiene que reservar en un solo pedazo de RAM, y ya no había un hueco tan grande.
  Ahora es una paleta de 16 colores con 4 bits por píxel (8,5 KB) que se dibuja fila por fila; a la
  vista queda igual. `tools/img2h.py --colors 16` genera ese formato.

## 0.1.3
- Intents (CardputerOS 0.7.0 and later, docs/intents.md): badge.get_url, badge.set_url.

## 0.1.2
- New vector launcher icon (CardputerOS 0.6.0 and later; 0.5.0 icons were bitmaps).

## 0.1.1
- Launcher icon (shown by CardputerOS 0.5.0 and later).

## [0.1.0]
- Logo de la Ekoparty 22 (130×130, RGB565) y QR a https://amasetti.com.
- Cualquier tecla alterna entre logo + QR y QR a pantalla completa.
