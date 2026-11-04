#!/usr/bin/env python3
"""
Маленький однобитный логотип для экрана выключения — из той же картинки, что и большой.

Большой логотип (main/logo4g.h) четырёхградационный и занимает экран целиком: он рисуется
вендорской последовательностью display4Gray, в которую текст не вписать. Экран выключения
однобитный, и текст на нём — главное, поэтому логотип приводится к 1 биту и уменьшается.

Источник — РАСПАКОВАННЫЕ плоскости большого логотипа, а не исходный JPEG: так на стекле окажется
ровно та картинка, которую прибор уже показывает при первом включении, без второго пути от
художника к пикселям.

Уменьшение — усреднением площади с последующим порогом, а не выборкой: у логотипа тонкие линии,
и выборка их рвёт. Порог высокий (любое заметное покрытие становится чернилами), иначе контур
дома распадается на пунктир.
"""
import re, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.join(HERE, "..", "firmware", "main")
SRC  = open(os.path.join(MAIN, "logo4g.h"), encoding="utf-8", errors="replace").read()
W, H = 400, 300

def arr(name):
    m = re.search(re.escape(name) + r"\[\d+\]\s*=\s*\{(.*?)\};", SRC, re.S)
    return bytes(int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(1)))

def unrle(s, n=15000):
    out = bytearray(); i = 0
    while i < len(s) and len(out) < n:
        c = s[i]; i += 1
        if c & 0x80: out += bytes([s[i]]) * ((c & 0x7f) + 1); i += 1
        else: k = c + 1; out += s[i:i+k]; i += k
    return bytes(out)

p24, p26 = unrle(arr("logo4g_plane24_rle")), unrle(arr("logo4g_plane26_rle"))
LEV = {(1,1):255, (1,0):170, (0,1):85, (0,0):0}
gray = bytearray(W*H)
for y in range(H):
    for x in range(W):
        b = y*50 + x//8; sh = 7 - (x % 8)
        gray[y*W+x] = LEV[((p24[b] >> sh) & 1, (p26[b] >> sh) & 1)]

# Обрезка по чернилам: поля в исходной картинке нам не нужны, их зададим раскладкой экрана.
rows = [y for y in range(H) if any(gray[y*W+x] < 200 for x in range(W))]
cols = [x for x in range(W) if any(gray[y*W+x] < 200 for y in range(H))]
x0, x1, y0, y1 = min(cols), max(cols), min(rows), max(rows)
sw, sh_ = x1-x0+1, y1-y0+1

TH = int(sys.argv[2]) if len(sys.argv) > 2 else 215
DH = int(sys.argv[1]) if len(sys.argv) > 1 else 130
DW = max(1, round(sw * DH / sh_))

out = bytearray(DW*DH)
for dy in range(DH):
    for dx in range(DW):
        sy0, sy1 = y0 + dy*sh_//DH, y0 + (dy+1)*sh_//DH
        sx0, sx1 = x0 + dx*sw//DW, x0 + (dx+1)*sw//DW
        if sy1 <= sy0: sy1 = sy0+1
        if sx1 <= sx0: sx1 = sx0+1
        n = (sy1-sy0)*(sx1-sx0)
        tot = sum(gray[y*W+x] for y in range(sy0, sy1) for x in range(sx0, sx1))
        out[dy*DW+dx] = 0 if tot/n < TH else 255

stride = (DW + 7)//8
bits = bytearray(stride*DH)
for y in range(DH):
    for x in range(DW):
        if out[y*DW+x] == 0: bits[y*stride + x//8] |= 0x80 >> (x % 8)

ink = sum(1 for v in out if v == 0)
print("исходник %dx%d -> %dx%d, порог %d, чернил %.1f%%, %d байт" %
      (sw, sh_, DW, DH, TH, 100.0*ink/(DW*DH), len(bits)))

if "--write" in sys.argv:
    with open(os.path.join(MAIN, "logo_small.h"), "w", encoding="utf-8") as f:
        f.write("// Сгенерирован Software/mockups/logo_small.py из main/logo4g.h. Руками не править.\n"
                "// Однобитный логотип для экрана выключения: тот же рисунок, что показывает прибор\n"
                "// при первом включении, приведённый к 1 биту и уменьшённый усреднением площади.\n"
                "// 1 = чернила, старший бит — левый пиксель, строки по %d байт.\n#pragma once\n#include <stdint.h>\n\n"
                % stride)
        f.write("#define LOGO_SMALL_W %d\n#define LOGO_SMALL_H %d\n#define LOGO_SMALL_STRIDE %d\n\n" % (DW, DH, stride))
        f.write("static const uint8_t logo_small[%d] = {\n" % len(bits))
        for i in range(0, len(bits), 16):
            f.write("    " + ", ".join("0x%02X" % b for b in bits[i:i+16]) + ",\n")
        f.write("};\n")
    print("записан main/logo_small.h")

# превью
import zlib, struct
def png(px, w, h, path):
    raw = b"".join(b"\x00" + bytes(px[y*w:(y+1)*w]) for y in range(h))
    def ch(t, d):
        c = t+d; return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" +
        ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)) + ch(b"IDAT", zlib.compress(raw)) + ch(b"IEND", b""))
png(out, DW, DH, os.path.join(HERE, "logo_small_preview.png"))
