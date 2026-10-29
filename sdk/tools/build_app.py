#!/usr/bin/env python3
"""Build one CardputerOS app into dist/<name>-v<version>.elf.

    python3 sdk/tools/build_app.py apps/snake [--out dist]

The app keeps its Arduino/M5Cardputer source untouched. Everything the OS needs to know
(name, version, permissions, stack) comes from the app's app.json; the .cpos_app header is
generated here.
"""
import argparse, glob, json, os, pathlib, shutil, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PERMS = {"sd": 1 << 0, "wifi": 1 << 1, "ble": 1 << 2, "audio_out": 1 << 3, "audio_in": 1 << 4,
         "imu": 1 << 5, "ir": 1 << 6, "gpio": 1 << 7}


def find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    for base in glob.glob(os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin")):
        c = os.path.join(base, name)
        if os.path.exists(c):
            return c
    sys.exit(f"{name} not found: install the ESP-IDF toolchain or put it in PATH")


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.stderr.write(" ".join(map(str, cmd))[:400] + "\n" + r.stdout + r.stderr)
        sys.exit(1)
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app")
    ap.add_argument("--out", default=str(ROOT / "dist"))
    ap.add_argument("--abi-minor", type=int, default=None)
    a = ap.parse_args()

    appdir = pathlib.Path(a.app).resolve()
    meta = json.loads((appdir / "app.json").read_text())
    name, version = meta["name"], meta["version"]
    perms = 0
    for p in meta.get("perms", []):
        perms |= PERMS[p]
    stack = int(meta.get("stack", 8192))
    abi_minor = a.abi_minor if a.abi_minor is not None else int(meta.get("abi_minor", 0))

    gcc = find_tool("xtensa-esp32s3-elf-gcc")
    gxx = find_tool("xtensa-esp32s3-elf-g++")
    strip = find_tool("xtensa-esp32s3-elf-strip")
    size = find_tool("xtensa-esp32s3-elf-size")

    build = pathlib.Path(a.out) / ".build" / name
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)

    hdr = build / "cpos_header.c"
    hdr.write_text(f'''#include <cpos/abi.h>
extern int cpos_app_entry(const cpos_api_t *api);
__attribute__((section(".cpos_app"), used)) const cpos_app_header_t cpos_app_header = {{
    .magic = CPOS_APP_MAGIC, .abi_major = CPOS_ABI_MAJOR, .abi_minor = {abi_minor},
    .perms = {perms}u, .stack_size = {stack}u, .entry = cpos_app_entry,
    .name = "{name}", .version = "{version}",
}};
''')

    common = ["-Os", "-mlongcalls", "-ffunction-sections", "-fdata-sections", "-fno-common",
              "-I", str(ROOT / "sdk/include"), "-I", str(ROOT / "sdk/shim/include"),
              "-I", str(appdir / "src"), "-DARDUINO=10812", "-DCPOS_APP=1", f'-DAPP_VERSION="{version}"',
              "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-psabi"]
    # _GLIBCXX_ASSERTIONS makes std::string instantiate in the app instead of importing libstdc++'s copy
    cxxflags = common + ["-std=gnu++17", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-Wno-reorder", "-D_GLIBCXX_ASSERTIONS"]
    cflags = common + ["-std=gnu17", "-fno-builtin"]

    sources = []
    for ext in ("*.cpp", "*.c", "*.cc"):
        sources += sorted(glob.glob(str(appdir / "src" / "**" / ext), recursive=True))
    shim = [ROOT / "sdk/shim/src/crt.cpp", ROOT / "sdk/shim/src/compat.cpp", ROOT / "sdk/shim/src/libc_stubs.c", hdr]
    objs = []
    for i, src in enumerate([*sources, *map(str, shim)]):
        obj = build / f"{i}_{pathlib.Path(src).stem}.o"
        is_cxx = src.endswith((".cpp", ".cc"))
        run([gxx if is_cxx else gcc, *(cxxflags if is_cxx else cflags), "-c", src, "-o", obj])
        objs.append(str(obj))

    libgcc = run([gcc, "-mlongcalls", "-print-libgcc-file-name"]).strip()
    elf = build / f"{name}.elf"
    run([gxx, "-nostdlib", "-nostartfiles", "-static", "-mlongcalls", f"-Wl,-T,{ROOT / 'sdk/app.ld'}",
         "-Wl,--gc-sections", "-Wl,--emit-relocs", "-Wl,--no-relax", f"-Wl,-Map,{build / (name + '.map')}",
         *objs, libgcc, "-o", elf])
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    final = out / f"{name}-v{version}.elf"
    run([strip, "--strip-debug", elf, "-o", final])
    print(run([size, final]).strip().splitlines()[-1])
    print(f"-> {final} ({final.stat().st_size} bytes)")


main()
