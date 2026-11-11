#!/usr/bin/env python3
"""Drives test/phone_server.cpp with the official Meshtastic Python client over TCP, as the phone
apps do over BLE: config handshake, node list, channel/LoRa config, a DM with ack, a reply.

    g++ -std=c++17 -Iapps/meshtastic/src apps/meshtastic/test/phone_server.cpp -lcrypto -o /tmp/ps
    pip install meshtastic
    python3 apps/meshtastic/test/phone_test.py /tmp/ps
"""
import subprocess, sys, threading, time
from pubsub import pub
import meshtastic.tcp_interface

srv = subprocess.Popen([sys.argv[1]], stdout=subprocess.PIPE, text=True)
for line in srv.stdout:
    print("server:", line.strip())
    if line.startswith("listening"):
        break

got_text, acks = [], []
def on_text(packet, interface):
    got_text.append(packet["decoded"]["text"])
def on_routing(packet, interface):
    acks.append((packet["decoded"].get("requestId"), packet["decoded"]["routing"].get("errorReason", "NONE")))
pub.subscribe(on_text, "meshtastic.receive.text")  # pubsub keeps weak references: named functions
pub.subscribe(on_routing, "meshtastic.receive.routing")

iface = meshtastic.tcp_interface.TCPInterface(hostname="127.0.0.1")
me = iface.myInfo.my_node_num
print("my node", hex(me), "firmware", iface.metadata.firmware_version)
print("nodes", {k: v["user"]["longName"] for k, v in iface.nodes.items()})
lora = iface.localNode.localConfig.lora
print("lora region", lora.region, "preset", lora.modem_preset, "hops", lora.hop_limit)
ch = iface.localNode.channels[0]
print("channel 0 role", ch.role, "psk", ch.settings.psk.hex())
assert me == 0xA1A2A3A4
assert "!b1b2b3b4" in iface.nodes and iface.nodes["!b1b2b3b4"]["user"]["longName"] == "Nodo Rosario"
assert lora.region == 6 and ch.role == 1 and ch.settings.psk == b"\x01"

sent = iface.sendText("ping", destinationId="!b1b2b3b4", wantAck=True)
for _ in range(150):
    if got_text and acks:
        break
    time.sleep(0.1)
print("ack", acks, "received", got_text)
assert acks == [(sent.id, "NONE")], (acks, sent.id)  # exactly one ack, for our packet
assert got_text == ["pong"], got_text

# a channel message: acked implicitly when a neighbour relays it
acks.clear()
bc = iface.sendText("hola canal", wantAck=True)
for _ in range(100):
    if acks:
        break
    time.sleep(0.1)
print("broadcast ack", acks)
assert acks == [(bc.id, "NONE")], (acks, bc.id)
# configuration from the phone (AdminMessage to the local node)
iface.localNode.setOwner(long_name="Cardputer Rosario", short_name="CROS")
lora = iface.localNode.localConfig.lora
lora.hop_limit = 5
iface.localNode.writeConfig("lora")
time.sleep(1)
iface.localNode.requestConfig(iface.localNode.localConfig.DESCRIPTOR.fields_by_name["lora"])
time.sleep(1)
print("lora hop_limit after write + re-read:", iface.localNode.localConfig.lora.hop_limit)
assert iface.localNode.localConfig.lora.hop_limit == 5
iface.close()
out = srv.communicate(timeout=10)[0] if False else ""
srv.terminate()
print("PHONE API OK")
