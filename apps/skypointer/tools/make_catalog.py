#!/usr/bin/env python3
"""Genera src/catalog.h para la app skypointer.

Entradas (se bajan una sola vez):
  hygdata_v41.csv           https://github.com/astronexus/HYG-Database (CC BY-SA 4.0)
  constellations.lines.json https://github.com/ofrohn/d3-celestial (BSD)

Salida: src/catalog.h con las estrellas como vectores unitarios J2000 en
int16 (para que el firmware solo tenga que rotarlos) y las líneas de
constelaciones como pares de esos mismos vectores.

  python3 tools/make_catalog.py --hyg /ruta/hygdata_v41.csv \
      --lines /ruta/constellations.lines.json --mag 5.5
"""

import argparse
import csv
import json
import math
import os

SCALE = 32767.0
HDR = """// Generado por tools/make_catalog.py -- no editar a mano.
// Estrellas: HYG Database v4.1 (astronexus) CC BY-SA 4.0, epoch J2000.
// Líneas de constelaciones: d3-celestial (ofrohn) BSD.
#pragma once
#include <stdint.h>

struct SkyStar {
    int16_t x, y, z;   // vector unitario ecuatorial J2000 (escala 32767)
    uint8_t mag10;     // magnitud * 10 + 20  (0 => mag -2.0)
    uint8_t ci10;      // índice de color B-V * 10 + 20 (0 => -2.0)
};

struct SkySeg { int16_t a[3], b[3]; };
"""


def unit(ra_deg, dec_deg):
    ra, dec = math.radians(ra_deg), math.radians(dec_deg)
    return (math.cos(dec) * math.cos(ra), math.cos(dec) * math.sin(ra), math.sin(dec))


def q(v):
    return tuple(max(-32767, min(32767, int(round(c * SCALE)))) for c in v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hyg", required=True)
    ap.add_argument("--lines", required=True)
    ap.add_argument("--mag", type=float, default=5.5)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "src", "catalog.h"))
    args = ap.parse_args()

    stars = []
    with open(args.hyg, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            try:
                mag = float(row["mag"])
            except (TypeError, ValueError):
                continue
            if row["proper"] == "Sol":
                continue
            named = bool(row["proper"]) or bool(row["bayer"])
            if mag > args.mag and not (named and mag <= args.mag + 1.2):
                continue
            ra_h = float(row["ra"])
            dec = float(row["dec"])
            v = unit(ra_h * 15.0, dec)
            ci = float(row["ci"]) if row["ci"] else 0.0
            stars.append((mag, v, ci))

    stars.sort(key=lambda s: s[0])

    lines = []
    with open(args.lines, encoding="utf-8") as fh:
        geo = json.load(fh)
    for feat in geo["features"]:
        geom = feat["geometry"]
        parts = geom["coordinates"] if geom["type"] == "MultiLineString" else [geom["coordinates"]]
        for part in parts:
            for a, b in zip(part, part[1:]):
                va = unit(a[0] % 360.0, a[1])
                vb = unit(b[0] % 360.0, b[1])
                lines.append((q(va), q(vb)))

    out = [HDR, "\nstatic const SkyStar SKY_STARS[] = {"]
    for mag, v, ci in stars:
        x, y, z = q(v)
        m = max(0, min(255, int(round(mag * 10 + 20))))
        c = max(0, min(255, int(round(ci * 10 + 20))))
        out.append(f"{{{x},{y},{z},{m},{c}}},")
    out.append("};\n")
    out.append(f"static const uint16_t SKY_STAR_COUNT = {len(stars)};\n")
    out.append("static const SkySeg SKY_SEGS[] = {")
    for a, b in lines:
        out.append("{{%d,%d,%d},{%d,%d,%d}}," % (a + b))
    out.append("};\n")
    out.append(f"static const uint16_t SKY_SEG_COUNT = {len(lines)};\n")

    path = os.path.abspath(args.out)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))
    print(f"{len(stars)} estrellas (mag <= {args.mag} + nombradas), {len(lines)} segmentos -> {path}")
    print(f"tamaño aprox: {len(stars) * 8 / 1024:.1f} KB estrellas, {len(lines) * 12 / 1024:.1f} KB líneas")


if __name__ == "__main__":
    main()
