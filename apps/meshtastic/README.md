# Meshtastic

Nodo [Meshtastic](https://meshtastic.org) en el Cardputer-Adv con el **Cap LoRa-1262** de M5Stack
(SX1262 + GNSS ATGM336H). Habla con cualquier nodo Meshtastic 2.x: celulares con la app oficial,
T-Beam, Heltec, RAK, otros Cardputer.

Necesita CardputerOS 0.8.0 o posterior (ABI 1.5: servicio de radio LoRa y GNSS en el OS).

## Argentina

| Ajuste | Valor de fábrica | Por qué |
|---|---|---|
| Región | **ANZ** (915-928 MHz) | ENACOM habilita 915-928 MHz para equipos de espectro ensanchado; es el plan que usa la red argentina |
| Modo | **LongFast** (SF11, 250 kHz, 4/5) | el preset por defecto de Meshtastic |
| Frecuencia | **919.875 MHz** | slot que sale del nombre del canal "LongFast" en ANZ |
| Canal | público (`AQ==`) | el canal por defecto de toda la red |
| Potencia | 22 dBm | el máximo del SX1262 |

La región **US** (902-928) está como opción para grupos que la usen, pero su slot por defecto
(906.875 MHz) cae en el segmento 905-915 MHz que Argentina asignó a otros servicios. Para hablar con
otro nodo, los dos tienen que estar en la misma región, modo y canal.

El módulo cubre 868-923 MHz: con un canal propio, conviene elegir un slot por debajo de 923 MHz
(Config > Slot de frecuencia).

## Uso

| Tecla | |
|---|---|
| Tab | Chat / Nodos / Config |
| Fn + `;` `.` | arriba / abajo |
| Fn + `,` `/` | Chat: conversación anterior / siguiente · Config: cambiar el valor |
| Enter | Chat: enviar · Nodos: mensaje directo al nodo elegido · Config: editar o aplicar |
| Espacio | Nodos: detalle (SNR, saltos, batería, posición, distancia y rumbo) |
| Fn + `` ` `` | salir |

* **Chat**: el canal y una conversación por cada nodo con el que hubo mensajes directos.
  Un mensaje directo muestra ✓ cuando llega el ack del destino; uno al canal muestra
  "✓ retransmitido" cuando un vecino lo repite.
* **Nodos**: los oídos más recientemente primero, con saltos o SNR y, si los dos tienen GPS, la distancia.
* **Config**: nombre largo y corto, región, modo, canal y clave (PSK en base64, 1, 16 o 32 bytes),
  slot, potencia, saltos, retransmitir, GPS, compartir posición (apagado de fábrica), sonido.

El nodo se anuncia (NodeInfo) al arrancar y cada 3 h, manda telemetría de batería cada 30 min y, si se
activa "Compartir posición" y hay fix, la posición cada 15 min. Retransmite los paquetes de los demás
(rol CLIENT, inundación administrada), también los de canales que no puede leer.

Ajustes en la flash; nodos y los últimos 40 mensajes en la SD (`/meshtastic`).

## Intents

`mesh.send` (`text`, `to` opcional: `!a1b2c3d4` o nombre corto), `mesh.listen` (`seconds`) y
`mesh.nodes`. Ver [docs/intents.md](../../docs/intents.md).

## Limitaciones

* Sin PKI (mensajes directos con Curve25519): este nodo no publica clave pública, así que los demás le
  escriben con la clave del canal, que el firmware acepta.
* Un solo canal (el primario); sin MQTT, traceroute como destino ni administración remota.
* Probado en el emulador (UI) y con tests de host contra los protobufs oficiales; falta validación en
  el aire con el Cap LoRa-1262.

## Tests

```bash
g++ -std=c++17 -Iapps/meshtastic/src apps/meshtastic/test/mesh_test.cpp -lcrypto -o /tmp/t && /tmp/t
# opcional: compara los paquetes con los protobufs oficiales y un AES-CTR independiente
pip install meshtastic cryptography
/tmp/t --dump | python3 apps/meshtastic/test/crosscheck.py
```
