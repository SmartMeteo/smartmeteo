#!/usr/bin/env python3
"""The Wi-Fi setup screen as firmware art: the picture WITHOUT its QR code and password line, plus
what the firmware needs to put this device's own ones back (main/portal_screen.cpp).

    python3 portal_art.py [--font Montserrat-Bold.ttf] [--sample PASSWORD]

Reads   portal_v4.png                       the approved screen (QR and password baked in)
        ../firmware/main/wifi_manager.h      the password alphabet and length
Writes  ../firmware/main/portal_art.h        geometry and declarations
        ../firmware/main/portal_art.cpp      blank 4-gray planes, label and glyphs
        portal_art_preview.png              the screen composed here with --sample, 2x

The password value is set in the picture's own bold face (Montserrat Bold); --font picks the file.
Its size and baseline are not typed in: they are fitted to the "SmartMeteo123" that portal_v4.png
carries, by rendering that string and matching it against the picture. The label "Password:" is
cut out of the picture itself, so it stays identical to the "Wi-Fi:" line above it.
"""
import argparse, os, re, sys
import numpy as np
from PIL import Image, ImageFont, ImageDraw

import rle_pack   # тот же формат, что читает main/rle.h

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.join(HERE, "..", "firmware", "main")
W, H = 400, 300
SS = 6                      # supersampling: portal_v4 itself came from a 6x design, LANCZOS-reduced

# Geometry measured on portal_v4.png (see the measurements in the 2026-09-17 journal).
QR_X, QR_Y, QR_MOD, QR_VER = 38, 80, 4, 3          # 29x29 modules, 4 px each
QR_CLEAR = (22, 64, 169, 211)                      # modules + 4-module quiet zone, inside the frame
LINE_TOP, LINE_H = 160, 26                         # the password line's cell (ink 166..177 + descenders)
LINE_CLEAR = (178, LINE_TOP, 399, LINE_TOP + LINE_H - 1)
LINE_CX = 283                                      # the line's centre in the approved picture
LABEL_X0, LABEL_X1 = 189, 259                      # "Password:" ink
VALUE_X0 = 265                                     # where the value began: the label's advance
OLD_VALUE, OLD_VALUE_X1 = "SmartMeteo123", 378
LINE_LEFT, LINE_RIGHT = 182, 396                   # never closer than 8 px to the QR frame (x 174)
PAD = 1                                            # glyph cell overhang on each side

def levels(gray):
    """0 black .. 3 white, the same rounding as gen_4gray.py."""
    return np.clip(np.round(np.asarray(gray, float) / 255 * 3), 0, 3).astype(np.uint8)

def header_value(name):
    """A #define from wifi_manager.h: a bare number, or string literals that may span backslash-
    continued lines and contain escapes — the alphabet holds a quote and a backslash of its own."""
    # Правила пароля — в ap_password.h, имя сети — по-прежнему в wifi_manager.h.
    t = (open(os.path.join(MAIN, "ap_password.h")).read()
         + open(os.path.join(MAIN, "wifi_manager.h")).read())
    m = re.search(r"#define\s+" + name + r"((?:[^\n]*\\\n)*[^\n]*)", t)
    if not m: sys.exit(f"{name} not found in ap_password.h / wifi_manager.h")
    body = m.group(1)
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    if parts:
        return "".join(parts).encode().decode("unicode_escape")
    return body.strip()

def render_line(font, text, baseline6, width):
    """text drawn at SS, reduced to 1x; returns the float gray image (255 white)."""
    im = Image.new("L", (width * SS, LINE_H * SS), 255)
    ImageDraw.Draw(im).text((PAD * SS, baseline6), text, font=font, fill=0, anchor="ls")
    return np.asarray(im.resize((width, LINE_H), Image.LANCZOS), float)

def fit_font(path, index, art):
    """Size and baseline that reproduce the value "SmartMeteo123" of the approved picture."""
    target = art[LINE_TOP:LINE_TOP + LINE_H, VALUE_X0 - PAD:VALUE_X0 - PAD + 130].astype(float)
    best = None
    for px6 in range(80, 110):                      # candidate sizes in 1/6 px
        font = ImageFont.truetype(path, px6, index=index)
        for base6 in range(14 * SS, 20 * SS):
            got = render_line(font, OLD_VALUE, base6, target.shape[1])
            err = float(((got - target) ** 2).mean())
            if best is None or err < best[0]: best = (err, px6, base6)
    return best

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default=None, help="bold face; default: Montserrat-Bold.ttf beside this script")
    ap.add_argument("--index", type=int, default=0, help="face index inside a .ttc")
    ap.add_argument("--sample", default=None, help="password for the preview (default: widest possible)")
    a = ap.parse_args()
    font_path = a.font or os.path.join(HERE, "fonts", "Montserrat-Bold.ttf")
    if not os.path.exists(font_path): sys.exit(f"font not found: {font_path}")

    alphabet = header_value("PORTAL_AP_PASS_ALPHABET")
    plen = int(header_value("PORTAL_AP_PASS_LEN"))

    art = np.asarray(Image.open(os.path.join(HERE, "portal_v4.png")).convert("L")).copy()

    err, px6, base6 = fit_font(font_path, a.index, art)
    font = ImageFont.truetype(font_path, px6, index=a.index)
    print(f"font {os.path.basename(font_path)}: size {px6 / SS:.2f} px, baseline {base6 / SS:.2f} px "
          f"below the line top, fit error {err:.1f}")

    # ---- label, cut from the picture ------------------------------------------------------------
    label = levels(art[LINE_TOP:LINE_TOP + LINE_H, LABEL_X0:LABEL_X1 + 1])
    label_adv6 = (VALUE_X0 - LABEL_X0) * SS

    # ---- glyphs ----------------------------------------------------------------------------------
    glyphs = []
    for ch in alphabet:
        adv6 = int(round(font.getlength(ch)))
        wcell = (adv6 + SS - 1) // SS + 2 * PAD + 1
        cell = levels(render_line(font, ch, base6, wcell))
        ink = np.where((cell < 3).any(axis=0))[0]
        glyphs.append((ch, cell, adv6, int(ink.max()) if len(ink) else 0))
    widest6 = max(g[2] for g in glyphs)
    worst = label_adv6 + widest6 * plen
    room = 2 * min(LINE_CX - LINE_LEFT, LINE_RIGHT - LINE_CX) * SS
    print(f"widest line {worst / SS:.1f} px of {room / SS:.0f} px available")
    if worst > room: sys.exit("the widest password does not fit its line")

    # ---- blank art -------------------------------------------------------------------------------
    blank = art.copy()
    for x0, y0, x1, y1 in (QR_CLEAR, LINE_CLEAR):
        blank[y0:y1 + 1, x0:x1 + 1] = 255
    lv = levels(blank)
    p24 = np.isin(lv, (1, 3)).astype(np.uint8)       # vendor table: black 0,0 dark 1,0 light 0,1 white 1,1
    p26 = np.isin(lv, (2, 3)).astype(np.uint8)

    # ---- header ----------------------------------------------------------------------------------
    px = bytearray()
    table = []
    def put(cell):
        off = len(px)
        flat = cell.flatten()
        for i in range(0, len(flat), 4):
            b = 0
            for k in range(4):
                v = int(flat[i + k]) if i + k < len(flat) else 3
                b = (b << 2) | v
            px.append(b)
        return off
    label_off = put(label)
    for ch, cell, adv6, ink_r in glyphs:
        table.append((ch, cell.shape[1], adv6, ink_r, put(cell)))

    packed_planes = {}          # имя -> (сжатое, sha256 исходника)

    def arr_rle(name, raw):
        import hashlib
        packed = rle_pack.encode(raw)
        if rle_pack.decode(packed, len(raw)) != raw:
            sys.exit(f"{name}: распаковка не совпала — ничего не записано")
        packed_planes[name] = (len(packed), hashlib.sha256(raw).hexdigest()[:16])
        return arr(f"{name}_rle", packed)

    def arr(name, data, per=16):
        s = f"const uint8_t {name}[{len(data)}] = {{\n"
        for i in range(0, len(data), per):
            s += "  " + ",".join(f"0x{x:02X}" for x in data[i:i + per]) + ",\n"
        return s + "};\n"
    # Плоскости жмутся здесь, до сборки заголовка: их длины и хеши нужны ему.
    for _nm, _pl in (("portal_art_plane24", p24), ("portal_art_plane26", p26)):
        _raw = np.packbits(_pl, axis=1).tobytes()
        packed_planes[_nm] = (len(rle_pack.encode(_raw)), __import__("hashlib").sha256(_raw).hexdigest()[:16])

    gen = ("// Auto-generated by Software/mockups/portal_art.py from portal_v4.png — do not edit.\n"
           f"// Password value face: {os.path.basename(font_path)}, {px6 / SS:.2f} px.\n")
    h = [
        gen.rstrip("\n"),
        "// The Wi-Fi setup screen without its QR code and password line, and what portal_screen.cpp needs",
        "// to draw this device's own. The data lives in portal_art.cpp, not here: with the initializers in",
        "// sight GCC folds memcpy() from these arrays into a second, anonymous copy of both planes (30 KB).",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"#define PORTAL_QR_X {QR_X}",
        f"#define PORTAL_QR_Y {QR_Y}",
        f"#define PORTAL_QR_MODULE {QR_MOD}",
        f"#define PORTAL_QR_VERSION {QR_VER}",
        f"#define PORTAL_LINE_TOP {LINE_TOP}",
        f"#define PORTAL_LINE_H {LINE_H}",
        f"#define PORTAL_LINE_CX {LINE_CX}",
        f"#define PORTAL_GLYPH_PAD {PAD}",
        f"#define PORTAL_SUBPX {SS}",
        f"#define PORTAL_LABEL_W {label.shape[1]}",
        f"#define PORTAL_LABEL_ADV {label_adv6}",
        f"#define PORTAL_LABEL_OFF {label_off}",
        f"#define PORTAL_GLYPH_COUNT {len(table)}",
        "",
        "// One cell per character: w columns x PORTAL_LINE_H rows, 2 bits a pixel (0 black .. 3 white),",
        "// MSB first. The cell starts PORTAL_GLYPH_PAD px left of the pen; adv is in 1/PORTAL_SUBPX px;",
        "// ink_r is the last inked column of the cell. The label \"Password:\" is the cell at PORTAL_LABEL_OFF.",
        "typedef struct { char ch; uint8_t w; uint16_t adv; uint8_t ink_r; uint16_t off; } portal_glyph_t;",
        "",
        f"extern const portal_glyph_t portal_glyphs[PORTAL_GLYPH_COUNT];",
        f"extern const uint8_t portal_glyph_px[{len(px)}];",
        "// Both planes ship run-length coded (main/rle.h): 30 KB of flat artwork packs to a quarter of",
        "// that, and portal_screen.cpp expands each into a caller's buffer. The lengths live here and the",
        "// data in the .cpp, for the same reason the glyphs do — an initializer in sight of a memcpy gets",
        "// folded into a second, anonymous copy.",
        f"#define PORTAL_ART_PLANE24_RLE_LEN {packed_planes['portal_art_plane24'][0]}",
        f"#define PORTAL_ART_PLANE26_RLE_LEN {packed_planes['portal_art_plane26'][0]}",
        f"// plane24 expands to 15000 bytes, sha256 {packed_planes['portal_art_plane24'][1]}",
        f"// plane26 expands to 15000 bytes, sha256 {packed_planes['portal_art_plane26'][1]}",
        "extern const uint8_t portal_art_plane24_rle[PORTAL_ART_PLANE24_RLE_LEN];",
        "extern const uint8_t portal_art_plane26_rle[PORTAL_ART_PLANE26_RLE_LEN];",
        "",
    ]
    open(os.path.join(MAIN, "portal_art.h"), "w").write("\n".join(h))
    c = [gen + '#include "portal_art.h"', "", "const portal_glyph_t portal_glyphs[PORTAL_GLYPH_COUNT] = {"]
    for ch, w, adv6, ink_r, off in table:
        # The character as a number, not a literal: the alphabet contains ' and \\, and both would
        # end the literal early and write a file that does not compile.
        shown = ch if ch.isalnum() else "\\x%02x" % ord(ch)
        c.append(f"  {{{ord(ch)}, {w}, {adv6}, {ink_r}, {off}}},   // {shown}")
    c.append("};")
    c.append("")
    body = "\n".join(c) + "\n" + arr("portal_glyph_px", px) + "\n"
    # Плоскости идут в образ сжатыми: 30 КБ плоской графики ужимаются вчетверо, а распаковывает их
    # main/rle.h в буфер кадра панели — без кучи и без отказа. Каждая проверяется распаковкой здесь же.
    for nm, plane in (("portal_art_plane24", p24), ("portal_art_plane26", p26)):
        raw = np.packbits(plane, axis=1).tobytes()
        body += arr_rle(nm, raw) + "\n"
    open(os.path.join(MAIN, "portal_art.cpp"), "w").write(body)
    print(f"portal_art.h/.cpp written: {len(alphabet)} glyphs, {len(px)} B of glyph pixels")

    # ---- preview: the same composition the firmware does ------------------------------------------
    sample = a.sample
    if sample is None:
        wide = max(glyphs, key=lambda g: g[2])[0]
        sample = wide * plen
    import qrcode
    from qrcode.constants import ERROR_CORRECT_L
    q = qrcode.QRCode(version=QR_VER, error_correction=ERROR_CORRECT_L, box_size=1, border=0)
    q.add_data(f"WIFI:S:{header_value('PORTAL_AP_SSID')};T:WPA;P:{sample};;")
    q.make(fit=False)
    pv = lv.copy()
    for y, row in enumerate(q.get_matrix()):
        for x, dark in enumerate(row):
            if dark: pv[QR_Y + y * QR_MOD:QR_Y + (y + 1) * QR_MOD, QR_X + x * QR_MOD:QR_X + (x + 1) * QR_MOD] = 0
    gl = {g[0]: g for g in glyphs}
    total6 = label_adv6
    for i, c in enumerate(sample):
        total6 += gl[c][2] if i < len(sample) - 1 else 0
    rnd = lambda s: (s + SS // 2) // SS               # nearest pixel, as portal_screen.cpp does
    total = rnd(total6) + gl[sample[-1]][3] + 1 - PAD
    x0 = LINE_CX - total // 2
    def blit(cell, x):
        h, w = cell.shape
        reg = pv[LINE_TOP:LINE_TOP + h, x:x + w]
        np.minimum(reg, cell[:, :reg.shape[1]], out=reg)
    blit(label, x0)
    pen6 = x0 * SS + label_adv6
    for c in sample:
        blit(gl[c][1], rnd(pen6) - PAD)
        pen6 += gl[c][2]
    Image.fromarray((pv * 85).astype(np.uint8)).resize((800, 600), Image.NEAREST) \
        .save(os.path.join(HERE, "portal_art_preview.png"))
    print(f"preview with '{sample}' -> portal_art_preview.png")

if __name__ == "__main__":
    main()
