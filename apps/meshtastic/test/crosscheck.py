#!/usr/bin/env python3
"""Cross check of mesh_test --dump against the official Meshtastic protobufs and an independent AES-CTR.

    pip install meshtastic cryptography
    g++ -std=c++17 -Iapps/meshtastic/src apps/meshtastic/test/mesh_test.cpp -lcrypto -o /tmp/t
    /tmp/t --dump | python3 apps/meshtastic/test/crosscheck.py

Every PKT line is decrypted the way the Meshtastic MQTT tools do it (nonce = packet id as 8 bytes LE +
sender as 8 bytes LE, AES-CTR) and parsed with meshtastic.protobuf.
"""
import struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from meshtastic.protobuf import mesh_pb2, mqtt_pb2, portnums_pb2, telemetry_pb2

key = None
seen = set()
for line in sys.stdin:
    tag, _, hexs = line.strip().partition(" ")
    data = bytes.fromhex(hexs)
    if tag == "KEY":
        key = data
        continue
    if tag == "ENV":  # encrypted packet as an MQTT gateway publishes it
        env = mqtt_pb2.ServiceEnvelope()
        env.ParseFromString(data)
        mp = env.packet
        assert env.channel_id == "LongFast" and env.gateway_id == "!0000cafe"
        nonce = mp.id.to_bytes(8, "little") + getattr(mp, "from").to_bytes(8, "little")
        inner = mesh_pb2.Data()
        inner.ParseFromString(Cipher(algorithms.AES(key), modes.CTR(nonce)).decryptor().update(mp.encrypted))
        assert getattr(mp, "from") == 0xCAFE and mp.channel == 8 and mp.hop_limit == 3 and mp.hop_start == 3 and mp.rx_rssi == -95
        print("ENVELOPE", portnums_pb2.PortNum.Name(inner.portnum), "via", env.gateway_id)
        seen.add("ENVELOPE")
        continue
    if tag == "MAP":
        env = mqtt_pb2.ServiceEnvelope()
        env.ParseFromString(data)
        assert env.packet.decoded.portnum == portnums_pb2.MAP_REPORT_APP
        m = mqtt_pb2.MapReport()
        m.ParseFromString(env.packet.decoded.payload)
        print("MAP_REPORT", m.long_name, m.short_name, m.latitude_i / 1e7, m.longitude_i / 1e7, m.position_precision,
              mesh_pb2.HardwareModel.Name(m.hw_model))
        assert m.position_precision == 14 and abs(m.latitude_i - -329442000) < (1 << 18) and m.hw_model == mesh_pb2.M5STACK_CARDPUTER_ADV
        seen.add("MAP_REPORT")
        continue
    if tag != "PKT":
        continue
    to, frm, pid, flags, ch, nh, relay = struct.unpack("<IIIBBBB", data[:16])
    nonce = pid.to_bytes(8, "little") + frm.to_bytes(8, "little")
    plain = Cipher(algorithms.AES(key), modes.CTR(nonce)).decryptor().update(data[16:])
    d = mesh_pb2.Data()
    d.ParseFromString(plain)
    port = portnums_pb2.PortNum.Name(d.portnum)
    assert ch == 8, f"channel hash {ch}"
    assert flags & 7 == 3 and flags >> 5 == 3, f"flags {flags:#x}"
    # MQTT gateways (firmware 2.5+) drop packets without the OK-to-MQTT bit, so the public maps never see them
    assert d.HasField("bitfield") and d.bitfield & 1, "OK_TO_MQTT bit missing"
    if d.portnum == portnums_pb2.TEXT_MESSAGE_APP:
        print(port, d.payload.decode())
        assert d.payload == b"dup"
    elif d.portnum == portnums_pb2.NODEINFO_APP:
        u = mesh_pb2.User()
        u.ParseFromString(d.payload)
        print(port, u.id, u.long_name, u.short_name, mesh_pb2.HardwareModel.Name(u.hw_model))
        assert u.long_name == "Cardputer AR" and u.hw_model == mesh_pb2.M5STACK_CARDPUTER_ADV
    elif d.portnum == portnums_pb2.POSITION_APP:
        p = mesh_pb2.Position()
        p.ParseFromString(d.payload)
        print(port, p.latitude_i / 1e7, p.longitude_i / 1e7, p.altitude, p.time)
        assert p.latitude_i == -329442000 and p.longitude_i == -606502000 and p.altitude == 31
    elif d.portnum == portnums_pb2.TELEMETRY_APP:
        t = telemetry_pb2.Telemetry()
        t.ParseFromString(d.payload)
        print(port, t.device_metrics.battery_level, round(t.device_metrics.voltage, 2), t.device_metrics.uptime_seconds)
        assert t.device_metrics.battery_level == 80 and t.device_metrics.uptime_seconds == 120
    seen.add(port)

expected = {"TEXT_MESSAGE_APP", "NODEINFO_APP", "POSITION_APP", "TELEMETRY_APP", "ENVELOPE", "MAP_REPORT"}
assert seen == expected, seen
print("crosscheck ok")
