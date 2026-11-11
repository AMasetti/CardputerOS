# Changelog — Brújula

## 0.2.0
- GPS del Cap LoRa-1262: satélites, velocidad y altitud en el pie de pantalla (sin el módulo, igual que antes).
- Vista Destino (tab): flecha, distancia y rumbo hacia un punto guardado o un nodo Meshtastic con posición,
  y cuánto girar. Para los nodos que mandan posición aproximada muestra el margen (±730 m en el canal público).
- Puntos: p guarda la posición actual, del borra el elegido; ; y . cambian de destino. Van a la SD en
  /compass/points.csv ("nombre,latitud,longitud"), editable en la PC.
- Nodos: los lee de /meshtastic/nodes.bin (lo que guardan las apps Meshtastic y Meshtastic BT).
- En 2D, el destino elegido aparece como un rombo verde sobre la rosa.
- Necesita CardputerOS 0.8.0 o posterior (GNSS); permisos nuevos: GNSS y tarjeta SD.

## 0.1.3
- Intents (CardputerOS 0.7.0 and later, docs/intents.md): compass.tilt, compass.flat.

## 0.1.2
- New vector launcher icon (CardputerOS 0.6.0 and later; 0.5.0 icons were bitmaps).

## 0.1.1
- Launcher icon (shown by CardputerOS 0.5.0 and later).

## [0.1.0]
- Fusión de acelerómetro y giróscopo (filtro de Mahony) para la orientación en 3D.
- Vista 3D: el Cardputer dibujado sobre una rosa de los vientos, con cámara detrás que sigue el rumbo.
- Vista 2D: dial clásico que gira, rumbo en grados y punto cardinal, nivel de burbuja e inclinación.
- El norte se fija con `enter` (el Cardputer-Adv no tiene magnetómetro) y se ajusta con `,` `/`.
- Calibración del giróscopo al arrancar y con `c`; corrección continua del sesgo mientras está quieto.
- `x` cambia la orientación del sensor por si los ejes no coinciden.
