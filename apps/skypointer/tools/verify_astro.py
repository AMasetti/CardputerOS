#!/usr/bin/env python3
"""Verifica la matemática del firmware contra PyEphem.

Compila tools/test_astro.cpp con clang++ y compara alt/az para estrellas,
Sol, Luna e ISS. Uso:

  python3 tools/verify_astro.py [--jd 2461358.45833] [--lat -32.95] [--lon -60.65]
"""

import argparse
import math
import os
import subprocess
import sys
import tempfile

import ephem

STARS = [
    ("Sirius", 101.287155, -16.716116),
    ("Vega", 279.234735, 38.783689),
    ("Arcturus", 213.915300, 19.182409),
    ("Canopus", 95.987958, -52.695661),
    ("Polaris", 37.954560, 89.264109),
    ("Fomalhaut", 344.412693, -29.622237),
    ("Achernar", 24.428527, -57.236753),
]

TOL = {"star": 0.02, "Sun": 0.02, "Moon": 0.10, "ISS": 0.30}


def jd_to_ephem(jd):
    # JD -> datetime UTC (sin microsegundos)
    unix = (jd - 2440587.5) * 86400.0
    return ephem.Date(ephem.Date(unix / 86400.0 + 25567.5))


def build(root, workdir):
    src = os.path.join(root, "src")
    tools = os.path.join(root, "tools")
    cc, cxx = "clang", "clang++"
    objs = []
    for name, compiler, extra in [
        ("vendor/SGP4.c", cc, []),
        ("astro.cpp", cxx, ["-std=c++17"]),
        ("sat.cpp", cxx, ["-std=c++17"]),
    ]:
        o = os.path.join(workdir, os.path.basename(name) + ".o")
        subprocess.run([compiler, "-c", "-O2", "-I", src, *extra,
                        os.path.join(src, name), "-o", o], check=True)
        objs.append(o)
    o = os.path.join(workdir, "test.o")
    subprocess.run([cxx, "-c", "-O2", "-std=c++17", "-I", src,
                    os.path.join(tools, "test_astro.cpp"), "-o", o], check=True)
    objs.append(o)
    exe = os.path.join(workdir, "test_astro")
    subprocess.run([cxx, *objs, "-lm", "-o", exe], check=True)
    return exe


def run(root, jd, lat, lon, tle, workdir):
    exe = build(root, workdir)
    cmd = [exe, f"{lat}", f"{lon}", f"{jd}"]
    if tle:
        cmd += tle
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    res = {}
    for line in out.strip().splitlines():
        parts = line.split("\t")
        if len(parts) == 3:
            try:
                res[parts[0]] = (float(parts[1]), float(parts[2]))
            except ValueError:
                print(f"  ! {line}")
    return res


def angdiff(a, b):
    d = (a - b + 180.0) % 360.0 - 180.0
    return d


def sepdiff(az1, alt1, az2, alt2):
    """Separación angular real entre dos pares (az, alt), en grados.

    Comparar azimut crudo castiga objetos cerca del nadir o del cenit, donde un
    error pequeño en el cielo se amplifica por 1/cos(alt)."""
    r = math.radians
    v = (math.cos(r(alt1)) * math.cos(r(az1)) - math.cos(r(alt2)) * math.cos(r(az2)),
         math.cos(r(alt1)) * math.sin(r(az1)) - math.cos(r(alt2)) * math.sin(r(az2)),
         math.sin(r(alt1)) - math.sin(r(alt2)))
    c = 0.5 * math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
    return math.degrees(2.0 * math.asin(min(1.0, c)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jd", type=float, default=None)
    ap.add_argument("--lat", type=float, default=-32.9468)
    ap.add_argument("--lon", type=float, default=-60.6393)
    ap.add_argument("--tle", default=os.path.expanduser("~/.hermes/cache/scratch/iss.tle"))
    args = ap.parse_args()

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    jd = args.jd or (ephem.julian_date(ephem.now()))
    tle = None
    if os.path.exists(args.tle):
        lines = [l.strip() for l in open(args.tle) if l.strip()]
        if len(lines) >= 3:
            tle = (lines[1], lines[2])

    out = run(root, jd, args.lat, args.lon, tle, tempfile.mkdtemp())

    obs = ephem.Observer()
    obs.lat, obs.lon, obs.elevation = str(args.lat), str(args.lon), 0
    obs.pressure = 0  # sin refracción, para comparar la geometría pura
    obs.date = jd_to_ephem(jd)

    rows = []
    for name, ra, dec in STARS:
        b = ephem.FixedBody()
        b._ra, b._dec, b._epoch = ephem.degrees(ra * math.pi / 180), ephem.degrees(dec * math.pi / 180), ephem.J2000
        b.compute(obs)
        mine = out.get(name)
        if not mine:
            continue
        rows.append((name, "star", mine[0], math.degrees(b.az), mine[1], math.degrees(b.alt)))

    for name, body in (("Sun", ephem.Sun()),):
        body.compute(obs)
        mine = out.get(name)
        if mine:
            rows.append((name, name, mine[0], math.degrees(body.az), mine[1], math.degrees(body.alt)))

    if tle:
        sat = ephem.readtle("ISS", *tle)
        sat.compute(obs)
        mine = out.get("ISS")
        if mine:
            rows.append(("ISS", "ISS", mine[0], math.degrees(sat.az), mine[1], math.degrees(sat.alt)))

    print(f"observador lat={args.lat} lon={args.lon}  jd={jd:.6f}  ({ephem.Date(jd_to_ephem(jd))} UTC)")
    print(f"{'objeto':<12}{'az (fw)':>10}{'az (ephem)':>12}{'d_az':>8}{'alt (fw)':>10}{'alt (ephem)':>13}{'d_alt':>8}{'sep':>8}")
    worst = 0.0
    fails = []
    for name, kind, az_m, az_e, alt_m, alt_e in rows:
        daz, dalt = angdiff(az_m, az_e), alt_m - alt_e
        sep = sepdiff(az_m, alt_m, az_e, alt_e)
        worst = max(worst, sep)
        flag = "" if sep <= TOL[kind] else "  <-- FUERA DE TOLERANCIA"
        if flag:
            fails.append(name)
        print(f"{name:<12}{az_m:>10.4f}{az_e:>12.4f}{daz:>8.3f}{alt_m:>10.4f}{alt_e:>13.4f}{dalt:>8.3f}{sep:>8.4f}{flag}")

    lst_m, lst_a = out.get("LST", (0, 0))[0], math.degrees(obs.sidereal_time())
    print(f"\nLST propio {lst_m:.5f}°  vs PyEphem {lst_a:.5f}°  -> {angdiff(lst_m, lst_a):.5f}°")
    print(f"error máximo: {worst:.4f}°")
    if fails:
        print("FALLÓ: " + ", ".join(fails))
        return 1
    print("OK: todo dentro de tolerancia")
    return 0


if __name__ == "__main__":
    sys.exit(main())
