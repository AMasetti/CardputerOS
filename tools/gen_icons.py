#!/usr/bin/env python3
"""Generate the app icons: apps/<name>/icon.bin (embedded in the ELF) and icon.png (preview).

    python3 tools/gen_icons.py            # needs Pillow for the previews

An icon is a tiny vector drawing, not a bitmap: the launcher draws it at any size with its own
primitives, in the theme colors (glyph = foreground, cut-outs = tile background). Coordinates are 0..100
over the icon box. icon.bin is a list of ops ending with a 0 byte; each op is `code | 0x80 if cut-out`
followed by its parameters (one byte each):
    1 RRECT x y w h r        2 CIRCLE x y r        3 ARC x y rin rout a0/2 a1/2 (degrees, 270 = up)
    4 TRI x0 y0 x1 y1 x2 y2  5 LINE x0 y0 x1 y1 thickness (round caps)
"""
import math, pathlib
from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parents[1]
RRECT, CIRCLE, ARC, TRI, LINE = 1, 2, 3, 4, 5


def R(x, y, w, h, r=0, cut=False): return (RRECT, cut, x, y, w, h, r)
def C(x, y, r, cut=False): return (CIRCLE, cut, x, y, r)
def A(x, y, rin, rout, a0, a1, cut=False): return (ARC, cut, x, y, rin, rout, a0 // 2, a1 // 2)
def T(x0, y0, x1, y1, x2, y2, cut=False): return (TRI, cut, x0, y0, x1, y1, x2, y2)
def L(x0, y0, x1, y1, t, cut=False): return (LINE, cut, x0, y0, x1, y1, t)


GLYPHS = {
    "robot-control": [  # droid head
        R(24, 38, 52, 38, 12), R(17, 49, 7, 16, 3), R(76, 49, 7, 16, 3),
        L(50, 38, 50, 26, 4), C(50, 23, 5),
        C(39, 54, 5, True), C(61, 54, 5, True), R(42, 64, 16, 3, 1, True)],
    "ble-sentinel": [  # radar
        A(50, 50, 28, 33, 0, 360), A(50, 50, 12, 15, 0, 360), L(50, 50, 73, 27, 3), C(66, 64, 4)],
    "mp3-player": [  # beamed notes
        C(38, 68, 9), C(66, 62, 9), R(44, 30, 5, 38), R(72, 24, 5, 38), L(46, 31, 74, 25, 7)],
    "shell-rc": [  # car
        R(20, 52, 60, 20, 7), R(32, 38, 36, 18, 7), R(37, 42, 12, 9, 2, True), R(52, 42, 12, 9, 2, True),
        C(35, 74, 10), C(35, 74, 4, True), C(65, 74, 10), C(65, 74, 4, True)],
    "wiz-rc": [  # bulb
        C(50, 42, 18), R(42, 52, 16, 18, 4), R(42, 72, 16, 5, 2), L(44, 63, 56, 63, 2, True),
        L(50, 14, 50, 21, 3), L(26, 22, 31, 27, 3), L(74, 22, 69, 27, 3), L(17, 42, 24, 42, 3), L(83, 42, 76, 42, 3)],
    "led-rc": [  # remote
        R(35, 17, 30, 66, 9), C(50, 31, 6, True),
        C(42, 48, 3, True), C(50, 48, 3, True), C(58, 48, 3, True),
        C(42, 58, 3, True), C(50, 58, 3, True), C(58, 58, 3, True),
        C(42, 68, 3, True), C(50, 68, 3, True), C(58, 68, 3, True)],
    "tv-b-gone": [  # TV with the power symbol
        R(19, 38, 62, 42, 8), R(25, 44, 50, 30, 4, True),
        A(50, 59, 6, 9, 0, 360), R(47, 49, 6, 8, 0, True), R(49, 50, 3, 10, 1),
        L(38, 36, 29, 25, 3), L(62, 36, 71, 25, 3), R(35, 82, 30, 3, 1)],
    "tuner": [  # gauge
        A(50, 66, 27, 32, 200, 340), L(50, 66, 63, 42, 4), C(50, 66, 6),
        L(26, 48, 31, 52, 3), L(50, 33, 50, 39, 3), L(74, 48, 69, 52, 3), R(30, 78, 40, 4, 2)],
    "compass": [  # needle in a ring
        A(50, 50, 29, 34, 0, 360), T(50, 24, 59, 50, 41, 50), T(50, 76, 59, 50, 41, 50), C(50, 50, 4, True)],
    "karting": [  # checkered flag
        R(25, 18, 5, 66, 2),
        R(32, 20, 12, 10, 1), R(56, 20, 12, 10, 1), R(44, 30, 12, 10, 1), R(68, 30, 12, 10, 1),
        R(32, 40, 12, 10, 1), R(56, 40, 12, 10, 1)],
    "snake": [  # snake and its food
        L(27, 72, 27, 52, 9), L(27, 52, 50, 52, 9), L(50, 52, 50, 32, 9), L(50, 32, 70, 32, 9),
        C(74, 32, 9), C(77, 29, 2, True), R(64, 66, 11, 11, 3)],
    "tetrix": [  # T piece, I piece
        R(12, 62, 16, 16, 3), R(31, 62, 16, 16, 3), R(50, 62, 16, 16, 3), R(31, 43, 16, 16, 3),
        R(69, 24, 16, 16, 3), R(69, 43, 16, 16, 3), R(69, 62, 16, 16, 3)],
    "ekoparty-badge": [  # conference badge
        R(30, 17, 40, 64, 8), R(43, 22, 14, 4, 2, True),
        R(37, 38, 10, 10, 1, True), R(53, 38, 10, 10, 1, True), R(37, 54, 10, 10, 1, True),
        R(53, 59, 10, 5, 1, True), R(40, 69, 20, 3, 1, True)],
}


def encode(ops):
    out = bytearray()
    for op in ops:
        out.append(op[0] | (0x80 if op[1] else 0))
        out += bytes(op[2:])
    out.append(0)
    return bytes(out)


def preview(ops, size=192):
    k = 4
    s = size * k
    bg, fg = (10, 30, 52), (64, 228, 255)
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=s // 5, fill=bg)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=s // 5, outline=fg, width=k * 2)
    gs, gx = s * 0.96, s * 0.02     # the glyph box sits inside the tile like the built-in icons
    P = lambda v: gx + v * gs / 100
    Z = lambda v: v * gs / 100
    for op in ops:
        code, cut, a = op[0], op[1], op[2:]
        col = bg if cut else fg
        if code == RRECT:
            d.rounded_rectangle([P(a[0]), P(a[1]), P(a[0] + a[2]), P(a[1] + a[3])], radius=Z(a[4]), fill=col)
        elif code == CIRCLE:
            d.ellipse([P(a[0]) - Z(a[2]), P(a[1]) - Z(a[2]), P(a[0]) + Z(a[2]), P(a[1]) + Z(a[2])], fill=col)
        elif code == ARC:
            x, y, ri, ro, a0, a1 = P(a[0]), P(a[1]), Z(a[2]), Z(a[3]), a[4] * 2, a[5] * 2
            if a1 - a0 >= 360:
                d.ellipse([x - ro, y - ro, x + ro, y + ro], fill=col)
                d.ellipse([x - ri, y - ri, x + ri, y + ri], fill=bg)
            else:
                m = Image.new("L", (s, s), 0)
                md = ImageDraw.Draw(m)
                md.pieslice([x - ro, y - ro, x + ro, y + ro], a0, a1, fill=255)
                md.ellipse([x - ri, y - ri, x + ri, y + ri], fill=0)
                img.paste(Image.new("RGBA", (s, s), col), (0, 0), m)
        elif code == TRI:
            d.polygon([(P(a[0]), P(a[1])), (P(a[2]), P(a[3])), (P(a[4]), P(a[5]))], fill=col)
        elif code == LINE:
            t = max(Z(a[4]), 2)
            d.line([(P(a[0]), P(a[1])), (P(a[2]), P(a[3]))], fill=col, width=int(t))
            for px, py in ((a[0], a[1]), (a[2], a[3])):
                d.ellipse([P(px) - t / 2, P(py) - t / 2, P(px) + t / 2, P(py) + t / 2], fill=col)
    return img.resize((size, size), Image.LANCZOS)


def main():
    for name, ops in GLYPHS.items():
        d = ROOT / "apps" / name
        if not d.is_dir():
            continue
        data = encode(ops)
        (d / "icon.bin").write_bytes(data)
        preview(ops).save(d / "icon.png")
        print(name, len(data))


if __name__ == "__main__":
    main()
