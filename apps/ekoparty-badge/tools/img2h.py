#!/usr/bin/env python3
"""Convierte una imagen a un header C para M5GFX.

    python3 tools/img2h.py assets/ekoparty22.jpg src/logo.h --size 130 --name LOGO --colors 16

Con --colors 16 (lo que usa la app) guarda una paleta RGB565 de 16 colores y 4 bits por
píxel: 8,5 KB en vez de 33,8 KB. Todo lo que es const termina en el bloque de datos de la app,
que el OS tiene que reservar en un solo pedazo de RAM; 34 KB contiguos ya no entraban.
Con --colors 0 genera el arreglo RGB565 completo de antes.

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


def write_indexed(img, a):
    q = img.quantize(colors=a.colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[: 3 * a.colors]
    pal += [0] * (3 * a.colors - len(pal))
    colors = [rgb565(*pal[i:i + 3]) for i in range(0, len(pal), 3)]
    idx = list(q.getdata())
    w = a.size
    # dos píxeles por byte, el de la izquierda en el nibble alto; cada fila empieza en un byte nuevo
    row_bytes = (w + 1) // 2
    packed = []
    for y in range(a.size):
        row = idx[y * w:(y + 1) * w] + [0]
        packed += [(row[x] << 4) | row[x + 1] for x in range(0, 2 * row_bytes, 2)]
    with open(a.dst, "w") as f:
        f.write(f"// Generado con tools/img2h.py desde {a.src.split('/')[-1]} (--colors {a.colors}). No editar a mano.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"static constexpr int {a.name}_W = {a.size};\n")
        f.write(f"static constexpr int {a.name}_H = {a.size};\n")
        f.write(f"static constexpr int {a.name}_ROW_BYTES = {row_bytes};\n")
        f.write(f"// paleta RGB565 y píxeles de 4 bits (nibble alto = píxel de la izquierda)\n")
        f.write(f"static const uint16_t {a.name}_PAL[{len(colors)}] = {{\n  " + ", ".join(f"0x{v:04X}" for v in colors) + ",\n};\n")
        f.write(f"static const uint8_t {a.name}_PX[{len(packed)}] = {{\n")
        for i in range(0, len(packed), 16):
            f.write("  " + ", ".join(f"0x{v:02X}" for v in packed[i:i + 16]) + ",\n")
        f.write("};\n")
    if a.preview:
        q.convert("RGB").save(a.preview)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--size", type=int, default=130, help="lado en píxeles")
    ap.add_argument("--name", default="LOGO")
    ap.add_argument("--bg", default="000000", help="color de fondo hex")
    ap.add_argument("--tol", type=int, default=40, help="tolerancia del blanco")
    ap.add_argument("--colors", type=int, default=16, help="16 = paleta y 4 bits por píxel; 0 = RGB565 completo")
    ap.add_argument("--preview", help="guardar un PNG de cómo queda")
    a = ap.parse_args()

    bg = tuple(int(a.bg[i:i + 2], 16) for i in (0, 2, 4))
    img = Image.open(a.src).convert("RGB")
    flood_background(img, bg, a.tol)
    img = img.resize((a.size, a.size), Image.LANCZOS)
    # Segunda pasada para limpiar el halo claro que deja el resize en el borde.
    flood_background(img, bg, a.tol)

    if a.colors:
        if not 2 <= a.colors <= 16:
            ap.error("--colors: entre 2 y 16, o 0")
        write_indexed(img, a)
        return

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
