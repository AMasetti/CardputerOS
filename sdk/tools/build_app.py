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


# reloc types that do not tie two places of the code together: absolute words (fixed by the
# loader wherever things land), call-expansion markers and debug differences
FREE_RELOCS = {"R_XTENSA_NONE", "R_XTENSA_32", "R_XTENSA_ASM_EXPAND", "R_XTENSA_ASM_SIMPLIFY",
               "R_XTENSA_DIFF8", "R_XTENSA_DIFF16", "R_XTENSA_DIFF32"}


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def pc_target(code, pc):
    """Target of the PC-relative Xtensa instruction at offset pc, or None if not recognised."""
    b = code[pc:pc + 3] + bytes(3)
    insn = b[0] | b[1] << 8 | b[2] << 16
    op0, n, m = insn & 15, (insn >> 4) & 3, (insn >> 6) & 3
    if op0 == 1:  # L32R: constant below the instruction
        return ((pc + 3) & ~3) + (((insn >> 8) - 0x10000) << 2)
    if op0 == 5:  # CALLn
        return (pc & ~3) + (sext(insn >> 6, 18) << 2) + 4
    if op0 == 6:
        if n == 0:  # J
            return pc + 4 + sext(insn >> 6, 18)
        if n == 1:  # BEQZ/BNEZ/BLTZ/BGEZ
            return pc + 4 + sext(insn >> 12, 12)
        if n == 2:  # BEQI/BNEI/BLTI/BGEI
            return pc + 4 + sext(insn >> 16, 8)
        if m == 1:  # BF/BT (signed), LOOP* (unsigned)
            r = (insn >> 12) & 15
            return pc + 4 + ((insn >> 16) if r >= 8 else sext(insn >> 16, 8))
        if m >= 2:  # BLTUI/BGEUI
            return pc + 4 + sext(insn >> 16, 8)
        return None
    if op0 == 7:  # register compare branches
        return pc + 4 + sext(insn >> 16, 8)
    if op0 == 12 and (insn >> 7) & 1:  # BEQZ.N/BNEZ.N
        return pc + 4 + ((((insn >> 4) & 3) << 4) | ((insn >> 12) & 15))
    return None


def code_splits(elf, mapfile, readelf):
    """Offsets where the code segment may be cut, so the OS can load it in several pieces.

    A cut must fall between two input sections (one function each, with its constants) and no
    PC-relative reference (jump, call, constant load) may cross it."""
    lines = pathlib.Path(mapfile).read_text().splitlines()
    i = next(n for n, l in enumerate(lines) if l.startswith(".text "))
    base, size = (int(x, 16) for x in lines[i].split()[1:3])
    starts, pend = set(), None
    for l in lines[i + 1:]:
        if l and not l[0].isspace():
            break  # next output section
        f = l.split()
        if len(f) == 1 and f[0].startswith("."):
            pend = f[0]
            continue
        if pend and len(f) >= 3 and f[0].startswith("0x") and f[1].startswith("0x"):
            addr, sz = int(f[0], 16), int(f[1], 16)
        elif len(f) >= 4 and f[0].startswith(".") and f[1].startswith("0x") and f[2].startswith("0x"):
            addr, sz = int(f[1], 16), int(f[2], 16)
        else:
            pend = None
            continue
        pend = None
        if sz and base < addr < base + size and addr % 4 == 0:
            starts.add(addr - base)

    # the relocations say which instructions are PC-relative; their addends are not usable
    # (relative to the input section), so the target is decoded from the linked instruction
    code = pathlib.Path(str(elf) + ".text.bin")
    run([find_tool("xtensa-esp32s3-elf-objcopy"), "-O", "binary", "--only-section=.text", elf, code])
    code = code.read_bytes()
    blocked = [0] * (size + 2)  # +1 at a, -1 at b: cutting at x is unsafe when a < x <= b
    section, unknown = None, 0
    for l in run([readelf, "-rW", elf]).splitlines():
        if l.startswith("Relocation section"):
            section = l.split("'")[1]
            continue
        if section != ".rela.text":
            continue
        f = l.split()
        if len(f) < 3 or not f[2].startswith("R_XTENSA") or f[2] in FREE_RELOCS:
            continue
        where = int(f[0], 16) - base
        target = pc_target(code, where)
        if target is None:
            unknown += 1
            target = where
        lo, hi = sorted((where, target))
        lo, hi = max(lo, 0), min(hi, size)
        blocked[lo + 1] += 1
        blocked[hi + 1] -= 1
    if unknown:
        print(f"note: {unknown} PC-relative instructions not decoded")
    ok, depth = [], 0
    for x in range(1, size + 1):
        depth += blocked[x]
        if x in starts and depth == 0:
            ok.append(x)
    return ok


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
    if a.abi_minor is None:
        a.abi_minor = int(meta.get("abi_minor", 0))
    # the ble and crypto tables arrived in minor 1: an app using them needs an OS that has them
    if "ble" in meta.get("perms", []):
        a.abi_minor = max(a.abi_minor, 1)
    # tasks, MP3 and JPEG decoding arrived in minor 2
    src_text = "".join(f.read_text(errors="ignore") for f in (appdir / "src").rglob("*") if f.suffix in (".c", ".cpp", ".h", ".hpp", ".ino"))
    if any(k in src_text for k in ("xTaskCreate", "AudioGeneratorMP3", "drawJpg", "cpos_api->rtos", "cpos_api->media")):
        a.abi_minor = max(a.abi_minor, 2)
    # stream sockets arrived in minor 3
    if "sock_connect" in src_text:
        a.abi_minor = max(a.abi_minor, 3)
    abi_minor = a.abi_minor

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

    # -mtext-section-literals keeps each function's constants inside its own section, so the
    # code can be cut between functions (see code_splits)
    common = ["-Os", "-mlongcalls", "-mtext-section-literals", "-ffunction-sections", "-fdata-sections", "-fno-common",
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
    # only the pieces still missing (map/set trees, hash tables) are taken from libstdc++
    libstdcxx = run([gxx, "-mlongcalls", "-fno-rtti", "-print-file-name=libstdc++.a"]).strip()
    elf = build / f"{name}.elf"
    run([gxx, "-nostdlib", "-nostartfiles", "-static", "-mlongcalls", f"-Wl,-T,{ROOT / 'sdk/app.ld'}",
         "-Wl,--gc-sections", "-Wl,--emit-relocs", "-Wl,--no-relax", f"-Wl,-Map,{build / (name + '.map')}",
         *objs, libstdcxx, libgcc, "-o", elf])
    # where the code may be cut, for a heap without one block big enough for all of it
    splits = code_splits(elf, build / (name + ".map"), find_tool("xtensa-esp32s3-elf-readelf"))
    (build / "splits.bin").write_bytes(b"".join(x.to_bytes(4, "little") for x in splits))
    run([find_tool("xtensa-esp32s3-elf-objcopy"), "--add-section", f".cpos_splits={build / 'splits.bin'}", elf])
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    final = out / f"{name}-v{version}.elf"
    run([strip, "--strip-debug", elf, "-o", final])
    print(run([size, final]).strip().splitlines()[-1])
    print(f"-> {final} ({final.stat().st_size} bytes)")


main()
