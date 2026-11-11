# Changelog

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
