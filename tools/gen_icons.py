#!/usr/bin/env python3
"""Generate the app icons: apps/<name>/icon.png (preview) and icon.bin (what build_app.py embeds).

    python3 tools/gen_icons.py            # needs Pillow

Style: dark navy tile, cyan outline, cyan line-art glyph with a soft glow.
icon.bin = 16 x RGB565 little endian palette + 48x48 pixels, 4 bits each (high nibble = left pixel).
Palette index 0 is transparent (the rounded corners). Edit the glyph functions below and rerun.
"""
import pathlib, struct, math
from PIL import Image, ImageDraw, ImageFilter

ROOT = pathlib.Path(__file__).resolve().parents[1]
S, N = 192, 48          # drawing size (4x supersampled) and stored size
CY = (64, 228, 255)     # cyan
CYL = (205, 248, 255)   # highlight
BG1, BG2 = (10, 30, 52), (5, 14, 30)
SW = 9                  # stroke width at 192
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def tile():
    img = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(img)
    for y in range(S):
        t = y / (S - 1)
        d.line([(0, y), (S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(BG1, BG2)) + (255,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=40, fill=255)
    img.putalpha(mask)
    return img, mask


def rr(d, box, r, w=SW, fill=None):
    d.rounded_rectangle(box, radius=r, outline=CY, width=w, fill=fill)


def line(d, pts, w=SW):
    if not isinstance(pts[0], (tuple, list)):
        pts = list(zip(pts[0::2], pts[1::2]))
    d.line(pts, fill=CY, width=w, joint="curve")
    for p in (pts[0], pts[-1]):  # round caps
        d.ellipse([p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2], fill=CY)


def dot(d, x, y, r, fill=CY):
    d.ellipse([x - r, y - r, x + r, y + r], fill=fill)


def circ(d, x, y, r, w=SW):
    d.ellipse([x - r, y - r, x + r, y + r], outline=CY, width=w)


# ---- glyphs, line art on the 192x192 canvas -------------------------------------------------
def g_robot(d):
    rr(d, [44, 72, 148, 148], 22)
    dot(d, 74, 108, 9); dot(d, 118, 108, 9)
    line(d, [78, 132, 114, 132], 7)
    line(d, [96, 72, 96, 44]); dot(d, 96, 38, 10)
    line(d, [30, 98, 30, 124]); line(d, [162, 98, 162, 124])


def g_sentinel(d):
    d.ellipse([30, 62, 162, 130], outline=CY, width=SW)
    circ(d, 96, 96, 19)
    dot(d, 96, 96, 7, CYL)
    for r in (62, 80):
        d.arc([96 - r, 96 - r, 96 + r, 96 + r], 252, 288, fill=CY, width=7)
        d.arc([96 - r, 96 - r, 96 + r, 96 + r], 72, 108, fill=CY, width=7)


def g_music(d):
    circ(d, 66, 130, 20); circ(d, 126, 116, 20)
    line(d, [86, 128, 86, 52]); line(d, [146, 114, 146, 38])
    line(d, [86, 52, 146, 38])


def g_car(d):
    d.line([(34, 116), (46, 92), (74, 70), (128, 70), (152, 96), (160, 116), (34, 116)], fill=CY, width=SW, joint="curve")
    line(d, [92, 76, 92, 98]); line(d, [92, 98, 140, 98], 7)
    for x in (68, 126):
        circ(d, x, 128, 20)


def g_bulb(d):
    d.arc([56, 40, 136, 120], 130, 50, fill=CY, width=SW)
    line(d, [72, 108, 77, 128]); line(d, [120, 108, 115, 128])
    line(d, [77, 128, 115, 128]); line(d, [81, 145, 111, 145]); line(d, [89, 160, 103, 160])
    for a in (-62, -31, 0, 31, 62):
        x1, y1 = 96 + 68 * math.sin(math.radians(a)), 80 - 68 * math.cos(math.radians(a))
        x2, y2 = 96 + 82 * math.sin(math.radians(a)), 80 - 82 * math.cos(math.radians(a))
        line(d, [x1, y1, x2, y2], 7)


def g_ledremote(d):
    rr(d, [58, 22, 134, 170], 24)
    dot(d, 96, 56, 11)
    for j in range(3):
        for i in range(3):
            circ(d, 78 + i * 18, 94 + j * 24, 6, 5)


def g_tv(d):
    rr(d, [26, 56, 166, 148], 16)
    d.arc([76, 78, 116, 118], -55, 235, fill=CY, width=8)
    line(d, [96, 76, 96, 98], 8)
    line(d, [72, 56, 52, 32], 7); line(d, [120, 56, 140, 32], 7)


def g_tuner(d):
    d.arc([26, 50, 166, 190], 215, 325, fill=CY, width=SW)
    line(d, [96, 122, 126, 84]); dot(d, 96, 122, 11)
    for a in (-35, 0, 35):
        x1, y1 = 96 + 62 * math.sin(math.radians(a)), 122 - 62 * math.cos(math.radians(a))
        x2, y2 = 96 + 76 * math.sin(math.radians(a)), 122 - 76 * math.cos(math.radians(a))
        line(d, [x1, y1, x2, y2], 6)
    pts = [(30 + i * 8.2, 160 + 12 * math.sin(i * 0.95)) for i in range(18)]
    line(d, pts, 7)


def g_compass(d):
    circ(d, 96, 96, 66)
    d.polygon([(96, 44), (114, 96), (96, 92), (78, 96)], outline=CY, fill=CY)
    d.polygon([(96, 148), (114, 96), (96, 100), (78, 96)], outline=CY, width=7)
    dot(d, 96, 96, 6, BG2)


def g_karting(d):
    line(d, [48, 30, 48, 164])
    for r in range(3):
        for c in range(4):
            x, y = 62 + c * 25, 34 + r * 25
            d.rectangle([x, y, x + 25, y + 25], outline=CY, width=5, fill=CY if (r + c) % 2 == 0 else None)


def g_snake(d):
    line(d, [34, 142, 34, 106, 90, 106, 90, 64, 134, 64], 14)
    circ(d, 142, 64, 18, 8)
    dot(d, 148, 58, 4)
    d.rectangle([122, 136, 148, 162], outline=CY, width=7)


def g_tetrix(d):
    def blk(cx, cy, fill=False):
        x, y = 30 + cx * 28, 30 + cy * 28
        d.rounded_rectangle([x, y, x + 24, y + 24], radius=5, outline=CY, width=6, fill=CY if fill else None)
    for cx, cy in [(1, 2), (0, 3), (1, 3), (2, 3)]:
        blk(cx, cy)
    for cx, cy in [(3, 2), (4, 2), (3, 3), (4, 3)]:
        blk(cx, cy, True)
    for cy in range(0, 2):
        blk(3, cy)


def g_badge(d):
    rr(d, [40, 24, 152, 170], 18)
    circ(d, 96, 52, 9, 6)
    for r in range(4):
        for c in range(4):
            if (r + 2 * c + r * c) % 3 != 1:
                d.rectangle([58 + c * 20, 86 + r * 20, 58 + c * 20 + 13, 86 + r * 20 + 13], fill=CY)


APPS = {
    "robot-control": g_robot, "ble-sentinel": g_sentinel, "mp3-player": g_music, "shell-rc": g_car,
    "wiz-rc": g_bulb, "led-rc": g_ledremote, "tv-b-gone": g_tv, "tuner": g_tuner, "compass": g_compass,
    "karting": g_karting, "snake": g_snake, "tetrix": g_tetrix, "ekoparty-badge": g_badge,
}


def palette():
    bg = [tuple(int(a + (b - a) * i / 2) for a, b in zip(BG1, BG2)) for i in range(3)]
    glow = [tuple(int(b + (c - b) * f) for b, c in zip(BG1, CY)) for f in (0.18, 0.35, 0.55)]
    return bg + glow + [(30, 130, 160), CY, CYL]   # 0-5 dithered background, then the line colors


def render(glyph):
    base, mask = tile()
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    glyph(d)
    # tile border
    d.rounded_rectangle([3, 3, S - 4, S - 4], radius=38, outline=(30, 130, 160, 255), width=6)
    glow = layer.getchannel("A").filter(ImageFilter.GaussianBlur(9)).point(lambda a: int(a * 0.55))
    g = Image.new("RGBA", (S, S), CY + (0,))
    g.putalpha(glow)
    base.alpha_composite(g)
    base.alpha_composite(layer)
    base.putalpha(mask)
    return base


def encode(img):
    small = img.resize((N, N), Image.BOX)
    alpha, rgb = small.getchannel("A"), small.convert("RGB")
    pal = palette()
    near = lambda q: min(range(len(pal)), key=lambda i: sum((u - v) ** 2 for u, v in zip(pal[i], q)))
    idx = []
    for y in range(N):
        for x in range(N):
            if alpha.getpixel((x, y)) < 128:
                idx.append(0)
                continue
            px = rgb.getpixel((x, y))
            k = near(px)
            if k < 6:  # background and glow: ordered dither gives a smooth falloff with few colors
                dth = (BAYER[y % 4][x % 4] - 7.5) * 0.9
                k = near(tuple(max(0, min(255, int(v + dth))) for v in px))
            idx.append(1 + k)
    out = bytearray()
    for i in range(16):
        r, g, b = (0, 0, 0) if i == 0 or i > len(pal) else pal[i - 1]
        out += struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    for i in range(0, len(idx), 2):
        out.append((idx[i] << 4) | idx[i + 1])
    prev = Image.new("RGBA", (N, N), (0, 0, 0, 0))   # what the device will draw
    for p, v in enumerate(idx):
        if v:
            prev.putpixel((p % N, p // N), pal[v - 1] + (255,))
    return bytes(out), prev


def main():
    for name, glyph in APPS.items():
        d = ROOT / "apps" / name
        if not d.is_dir():
            continue
        data, prev = encode(render(glyph))
        (d / "icon.bin").write_bytes(data)
        prev.resize((192, 192), Image.NEAREST).save(d / "icon.png")
        print(name, len(data))


if __name__ == "__main__":
    main()
