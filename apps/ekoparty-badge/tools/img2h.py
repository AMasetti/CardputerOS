#!/usr/bin/env python3
"""Convierte una imagen a un header C con un arreglo RGB565 para M5GFX.

    python3 tools/img2h.py assets/ekoparty22.jpg src/logo.h --size 130 --name LOGO

El fondo claro que toca los bordes (el blanco alrededor del logo) se rellena
con --bg para que se funda con la pantalla. Requiere Pillow.
"""
import argparse
from collections import deque

from PIL import Image


def flood_background(img, bg, tol):
    """Pinta de `bg` los píxeles casi blancos conectados a los bordes."""
    w, h = img.size
    px = img.load()
    seen = bytearray(w * h)
    q = deque()
    for x in range(w):
        q.extend(((x, 0), (x, h - 1)))
    for y in range(h):
        q.extend(((0, y), (w - 1, y)))
    while q:
        x, y = q.popleft()
        if not (0 <= x < w and 0 <= y < h) or seen[y * w + x]:
            continue
        seen[y * w + x] = 1
        if min(px[x, y]) < 255 - tol:
            continue
        px[x, y] = bg
        q.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--size", type=int, default=130, help="lado en píxeles")
    ap.add_argument("--name", default="LOGO")
    ap.add_argument("--bg", default="000000", help="color de fondo hex")
    ap.add_argument("--tol", type=int, default=40, help="tolerancia del blanco")
    ap.add_argument("--preview", help="guardar un PNG de cómo queda")
    a = ap.parse_args()

    bg = tuple(int(a.bg[i:i + 2], 16) for i in (0, 2, 4))
    img = Image.open(a.src).convert("RGB")
    flood_background(img, bg, a.tol)
    img = img.resize((a.size, a.size), Image.LANCZOS)
    # Segunda pasada para limpiar el halo claro que deja el resize en el borde.
    flood_background(img, bg, a.tol)

    data = [rgb565(*p) for p in img.getdata()]
    with open(a.dst, "w") as f:
        f.write(f"// Generado con tools/img2h.py desde {a.src.split('/')[-1]}. No editar a mano.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"static constexpr int {a.name}_W = {a.size};\n")
        f.write(f"static constexpr int {a.name}_H = {a.size};\n")
        f.write(f"static const uint16_t {a.name}[{len(data)}] = {{\n")
        for i in range(0, len(data), 12):
            f.write("  " + ", ".join(f"0x{v:04X}" for v in data[i:i + 12]) + ",\n")
        f.write("};\n")
    if a.preview:
        img.save(a.preview)


if __name__ == "__main__":
    main()
