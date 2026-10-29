#!/usr/bin/env python3
"""Fake Parrot Jumping Sumo + checks for sumo_core.h.

    python3 apps/robot-control/test/run_host_test.py

Builds sumo_test.cpp, plays a drone on localhost (discovery over TCP, frames over UDP, an MJPEG stream
split in fragments) and verifies what the client sent and what it reassembled.
"""
import io, json, os, pathlib, socket, struct, subprocess, sys, tempfile, threading, time
from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
work = pathlib.Path(tempfile.mkdtemp(prefix="sumo_test_"))
exe = work / "host_test"
subprocess.check_call(["g++", "-std=c++17", "-O1", "-Wall", "-o", str(exe), str(HERE / "sumo_test.cpp")])

FT_ACK, FT_DATA, FT_LOW, FT_DACK = 1, 2, 3, 4

def frame(t, i, seq, payload):
    return struct.pack("<BBBI", t, i, seq, 7 + len(payload)) + payload

def jpeg(n):
    im = Image.new("RGB", (640, 480))
    d = ImageDraw.Draw(im)
    for y in range(0, 480, 8):
        d.rectangle([0, y, 640, y + 8], fill=((y * 255) // 480, (n * 40) % 256, 255 - (y * 255) // 480))
    d.text((20, 20), f"frame {n}", fill=(255, 255, 255))
    b = io.BytesIO()
    im.save(b, "JPEG", quality=40)
    return b.getvalue()

def free_port(kind):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p

disc, c2d, d2c = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_DGRAM)
got = {"pcmd": [], "cmds": [], "acks": [], "pong": 0, "videoacks": {}}
sent_jpegs = {}
stop = False

def discovery():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", disc)); s.listen(1); s.settimeout(10)
    c, _ = s.accept()
    got["hello"] = c.recv(512).rstrip(b"\0").decode()
    c.send(json.dumps({"status": 0, "c2d_port": c2d, "c2d_update_port": 51, "arstream_fragment_size": 1000}).encode() + b"\0")
    c.close()

def drone():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("127.0.0.1", c2d)); u.settimeout(0.01)
    out = ("127.0.0.1", d2c)
    seqs = {}
    def tx(t, i, p):
        seqs[i] = (seqs.get(i, 0) + 1) & 255
        u.sendto(frame(t, i, seqs[i], p), out)
    t0 = time.time(); step = 0; fno = 0
    while not stop:
        try:
            d, _ = u.recvfrom(2048)
            pos = 0
            while pos + 7 <= len(d):
                t, i, sq, size = struct.unpack_from("<BBBI", d, pos)
                p = d[pos + 7: pos + size]
                pos += size
                if i == 10 and p[:4] == bytes([3, 0, 0, 0]): got["pcmd"].append(struct.unpack("<Bbb", p[4:7]))
                elif i == 11: got["cmds"].append(bytes(p))
                elif i == 13: got["videoacks"][struct.unpack("<H", p[:2])[0]] = bytes(p[2:])
                elif t == FT_ACK: got["acks"].append((i, p[0]))
                elif i == 1: got["pong"] += 1
        except socket.timeout:
            pass
        el = time.time() - t0
        if step == 0 and el > 0.5:   # battery + posture in ONE datagram, plus a ping
            ev1 = frame(FT_DACK, 126, 1, bytes([0, 5, 1, 0, 85]))
            ev2 = frame(FT_DACK, 126, 2, bytes([3, 1, 0, 0]) + struct.pack("<I", 1))
            u.sendto(ev1 + ev2, out)
            u.sendto(frame(FT_DATA, 0, 1, struct.pack("<II", 7, 9)), out)
            step = 1
        if step == 1 and el > 1.0:   # a video frame every ~100 ms
            if time.time() - getattr(drone, "last", 0) > 0.1:
                drone.last = time.time(); fno += 1
                j = jpeg(fno); sent_jpegs[fno] = j
                frags = [j[k:k + 1000] for k in range(0, len(j), 1000)]
                for k, f in enumerate(frags):
                    tx(FT_LOW, 125, struct.pack("<HBBB", fno, 0, k, len(frags)) + f)
        if el > 5.5: break

th = [threading.Thread(target=discovery), threading.Thread(target=drone)]
for t in th: t.start()
out_dir = work / "frames"; out_dir.mkdir()
res = subprocess.run([str(exe), str(disc), str(d2c), str(out_dir)], capture_output=True, text=True, timeout=30)
stop = True
for t in th: t.join()
print(res.stdout.strip())

fails = []
def check(c, m):
    if not c: fails.append(m)
check('"controller_type"' in got.get("hello", ""), "handshake JSON not sent")
check(f'"d2c_port":"{d2c}"' in got.get("hello", ""), "d2c_port missing in handshake")
check(len(got["pcmd"]) > 100, f"too few PCMD frames: {len(got['pcmd'])}")
check(all(p == (1, 50, -20) for p in got["pcmd"][:50]), f"bad PCMD content {got['pcmd'][:3]}")
cmds = got["cmds"]
check(bytes([0, 4, 0, 0]) in cmds, "ALLSTATES not sent")
check(bytes([3, 18, 0, 0, 1]) in cmds, "VIDEOENABLE not sent")
check(bytes([3, 2, 3, 0, 1, 0, 0, 0]) in cmds, "high jump not sent")
check(bytes([3, 2, 4, 0, 1, 0, 0, 0]) in cmds, "spin animation not sent")
check(bytes([3, 0, 1, 0, 1, 0, 0, 0]) in cmds, "posture jumper not sent")
check((126 + 128, 1) in got["acks"] and (126 + 128, 2) in got["acks"], f"event acks missing: {got['acks']}")
check(got["pong"] >= 1, "no pong for the ping")
files = sorted(out_dir.glob("frame_*.jpg"), key=lambda f: int(f.stem.split("_")[1]))
check(len(files) >= 20, f"too few video frames reassembled: {len(files)}")
okimg = 0
for f in files:
    try:
        im = Image.open(f); im.load()
        okimg += im.size == (640, 480)
    except Exception as e:
        fails.append(f"{f.name} is not a valid JPEG: {e}")
check(okimg == len(files), "some frames are not 640x480")
# every reassembled frame must be byte-identical to what the drone sent
for k, f in enumerate(files, 1):
    if f.read_bytes() != sent_jpegs.get(k):
        fails.append(f"frame {k} differs from the original"); break
# ack masks: every fragment of a frame was acknowledged
full = 0
for fno, mask in got["videoacks"].items():
    j = sent_jpegs.get(fno)
    if not j: continue
    n = (len(j) + 999) // 1000
    bits = int.from_bytes(mask[8:16], "little") | (int.from_bytes(mask[0:8], "little") << 64)  # low then high on the wire
    full += bits == (1 << n) - 1
check(full >= 15, f"video ack masks incomplete: {full} frames fully acknowledged")
print("frames:", len(files), "fully acked:", full)
if fails:
    print("FAIL"); [print(" -", m) for m in fails]; sys.exit(1)
print("PASS")
