#!/usr/bin/env python3
"""Genera src/catalog.h para la app skypointer (CardputerOS).

Entradas (se bajan una sola vez):
  hygdata_v41.csv            https://github.com/astronexus/HYG-Database (CC BY-SA 4.0)
  constellations.lines.json  https://github.com/ofrohn/d3-celestial (BSD)

Salida: src/catalog.h con las estrellas como vectores unitarios J2000 en int16
(6 bytes: x, y, z, magnitud) y las líneas de constelaciones como pares de
índices a esas estrellas (4 bytes). Importa el tamaño: en CardputerOS el
.rodata de una app se copia a RAM al cargarla, y hay que pedirla en un solo
bloque contiguo.

  python3 tools/make_catalog.py --hyg /ruta/hygdata_v41.csv \
      --lines /ruta/constellations.lines.json --mag 4.8
"""

import argparse
import csv
import json
import math
import os

SCALE = 32767.0
MATCH_DEG = 0.45  # tolerancia para pegar el extremo de una línea a una estrella del catálogo
HDR = """// Generado por tools/make_catalog.py -- no editar a mano.
// Estrellas: HYG Database v4.1 (astronexus) CC BY-SA 4.0, epoch J2000, ordenadas por magnitud.
// Líneas de constelaciones: d3-celestial (ofrohn) BSD, pegadas a la estrella más cercana.
#pragma once
#include <stdint.h>

struct SkyStar {
    int16_t x, y, z;   // vector unitario ecuatorial J2000 (escala 32767)
    uint8_t mag10;     // magnitud * 10 + 20  (0 => mag -2.0)
};

// Extremos de un segmento: índices a SKY_STARS.
struct SkySeg { uint16_t a, b; };

// El catálogo queda en .rodata (segmento de datos): el OS lo pide en un solo
// bloque contiguo, así que tiene que ser chico. Y no puede ir al segmento de
// código porque ese también tiene un techo (IRAM) y ya estamos cerca.
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
    ap.add_argument("--mag", type=float, default=4.8)
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
            v = unit(float(row["ra"]) * 15.0, float(row["dec"]))
            stars.append((mag, v))

    stars.sort(key=lambda s: s[0])
    vecs = [s[1] for s in stars]

    def nearest(v):
        best, bi = -2.0, -1
        for i, w in enumerate(vecs):
            d = v[0] * w[0] + v[1] * w[1] + v[2] * w[2]
            if d > best:
                best, bi = d, i
        return bi, best

    limit = math.cos(math.radians(MATCH_DEG))
    segs, dropped = [], 0
    with open(args.lines, encoding="utf-8") as fh:
        geo = json.load(fh)
    for feat in geo["features"]:
        geom = feat["geometry"]
        parts = geom["coordinates"] if geom["type"] == "MultiLineString" else [geom["coordinates"]]
        for part in parts:
            for a, b in zip(part, part[1:]):
                ia, da = nearest(unit(a[0] % 360.0, a[1]))
                ib, db = nearest(unit(b[0] % 360.0, b[1]))
                if ia < 0 or ib < 0 or ia == ib or da < limit or db < limit:
                    dropped += 1
                    continue
                segs.append((ia, ib))

    out = [HDR, "\nstatic const SkyStar SKY_STARS[] = {"]
    for mag, v in stars:
        x, y, z = q(v)
        m = max(0, min(255, int(round(mag * 10 + 20))))
        out.append(f"{{{x},{y},{z},{m}}},")
    out.append("};\n")
    out.append(f"static const uint16_t SKY_STAR_COUNT = {len(stars)};\n")
    out.append("static const SkySeg SKY_SEGS[] = {")
    for a, b in segs:
        out.append("{%d,%d}," % (a, b))
    out.append("};\n")
    out.append(f"static const uint16_t SKY_SEG_COUNT = {len(segs)};\n")

    path = os.path.abspath(args.out)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))
    print(f"{len(stars)} estrellas (mag <= {args.mag} + nombradas), {len(segs)} segmentos "
          f"({dropped} descartados sin estrella cerca) -> {path}")
    print(f"tamaño en RAM: {len(stars) * 6 / 1024:.1f} KB estrellas + {len(segs) * 4 / 1024:.1f} KB líneas")


if __name__ == "__main__":
    main()
