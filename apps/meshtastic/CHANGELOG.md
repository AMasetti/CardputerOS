# Changelog

## 0.2.2
- La pantalla se dibuja en dos franjas de 240x68: el canvas usa 32 KB en vez de 64 KB, RAM que queda
  para el WiFi del gateway (antes: "sin memoria").
- El OS decide si hay RAM para el WiFi (CardputerOS 0.8.6); Config muestra la memoria libre si no alcanza.
- Ajuste de líneas en tiempo lineal (antes medía el texto una vez por carácter).
- Sin radio, el diagnóstico ya no tapa los mensajes del chat.

## 0.2.1
- Fix: un mensaje recortado o con UTF-8 mal formado (llega de la red) hacía leer más allá del texto
  al acomodarlo en líneas y podía colgar o reiniciar el sistema.
- Gateway WiFi: no intenta prender el WiFi con menos de 60 KB libres ("sí · sin memoria" en Config).
- Menos RAM: hasta 60 nodos, colas de radio más chicas.

## 0.2.0
- Gateway WiFi propio: con la red guardada en System > WiFi, el nodo publica en mqtt.meshtastic.org
  (msh/<región>/2/e/<canal>/<id>) sus paquetes cifrados, los de otros nodos que permiten MQTT, y un
  map report (msh/<región>/2/map/, posición con 14 bits) si "Compartir posición" está activo. Así
  aparece en los mapas públicos sin depender de otro gateway, incluso sin radio LoRa.
- Indicador MQTT en la barra superior; ajuste "Gateway WiFi" (sí de fábrica).
- Permiso nuevo: WiFi. Necesita CardputerOS 0.8.4 (ABI 1.7: DNS y conexión a la red guardada).

## 0.1.3
- Los paquetes llevan la marca "OK to MQTT" (Data.bitfield bit 0): sin ella los gateways con firmware
  2.5+ no los suben a MQTT y el nodo no aparece en los mapas públicos. Ajuste "Subir a MQTT/mapa" (sí de fábrica).
- En el canal público la posición sale con 15 bits de precisión (~700 m), como hace el firmware.

## 0.1.2
- Diagnóstico de radio más largo (hasta 4 líneas). Con CardputerOS 0.8.3 la radio arranca en el
  Cap LoRa-1262 (el OS descartaba el TCXO por un error de arranque que quedaba del encendido).

## 0.1.1
- Sin radio: muestra el diagnóstico del OS (expansor del cap, línea BUSY, registro del SX1262, GPS)
  y Enter (sin texto) reintenta. Necesita CardputerOS 0.8.2 (ABI 1.6).

## 0.1.0
- Primera versión. Nodo Meshtastic 2.x con el Cap LoRa-1262 (CardputerOS 0.8.0 o posterior, ABI 1.5).
- Región ANZ (915-928 MHz, la banda que ENACOM habilita en Argentina) y LongFast por defecto:
  919.875 MHz en el canal público. US (902-928) disponible para grupos que la usen.
- Chat en el canal y mensajes directos con confirmación de entrega; ack implícito en el canal cuando
  un vecino retransmite.
- Lista de nodos (nombre, saltos, SNR, batería, distancia y rumbo con GPS), NodeInfo cada 3 h,
  posición cada 15 min (opcional, apagada de fábrica), telemetría de batería cada 30 min.
- Retransmite paquetes de otros (inundación administrada, rol CLIENT), incluso de canales que no
  puede leer.
- Canal y clave configurables (PSK en base64), slot de frecuencia, potencia, saltos.
- Nodos y mensajes guardados en la SD (/meshtastic).
- Intents: mesh.send, mesh.listen, mesh.nodes.
- Sin PKI (mensajes directos cifrados con Curve25519): los demás nodos le escriben con la clave del canal.
