# Meshtastic BT

Nodo [Meshtastic](https://meshtastic.org) sin interfaz para usar con la **app oficial** (iPhone o Android)
por Bluetooth. El Cardputer-Adv con el **Cap LoRa-1262** pone la radio LoRa, el GPS y la retransmisión;
el teléfono es la pantalla: chat, lista de nodos, mapa y configuración.

Necesita CardputerOS 0.9.0 o posterior (ABI 1.8: servidor Bluetooth para apps del teléfono).

## Uso

1. Abrir **Meshtastic BT** en el Cardputer. La pantalla muestra los logos y dos puntos:

   | Punto | Color | Significa |
   |---|---|---|
   | abajo a la izquierda (Bluetooth) | gris | esperando al teléfono |
   | | azul | teléfono conectado |
   | | rojo | el Bluetooth no arrancó |
   | abajo a la derecha (LoRa) | verde | radio lista |
   | | rojo | no responde el Cap LoRa-1262 |

2. En la app Meshtastic del teléfono: conectar por Bluetooth y elegir el nodo `<nombre corto>_<4 hex>`
   (por ejemplo `2501_6c40`). No pide PIN.
3. Desde la app: mensajes al canal y directos, nodos, mapa. En Configuración se cambian el nombre,
   la región (ANZ o US), el modo, el canal y la clave; el Cardputer los aplica y los guarda.

Fn + `` ` `` sale de la app (el teléfono se desconecta).

## Con la app Meshtastic (con pantalla)

Las dos apps son el mismo nodo: comparten identidad, nombre, radio, canal y lista de nodos por la SD
(`/meshtastic/settings.bin` y `nodes.bin`). Se usa una u otra; los mensajes de esta versión quedan en el
teléfono.

## Limitaciones

* Sin PKI: los mensajes directos van con la clave del canal (como en la otra app).
* Desde el teléfono se aplican el nombre, la configuración LoRa, el canal primario y la hora; el resto de
  los ajustes se acepta y se ignora. Regiones fuera de 902-928 MHz se ignoran (el módulo no las cubre).
* Sin gateway MQTT (el WiFi y el Bluetooth no entran juntos en la RAM).
* Probado con el cliente oficial de Python sobre TCP (mismo protocolo que el Bluetooth) y en el emulador;
  el Bluetooth en sí falta probarlo con el teléfono.

## Tests

```bash
g++ -std=c++17 -Iapps/meshtastic/src apps/meshtastic/test/phone_server.cpp -lcrypto -o /tmp/ps
pip install meshtastic
python3 apps/meshtastic/test/phone_test.py /tmp/ps
```
