#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Сжатие четырёхсерых картинок для прошивки — тот же формат, что читает main/rle.h.

    rle_pack.py ../firmware/main/logo4g.h logo4g_plane24 logo4g_plane26

Переписывает массивы в файле: вместо `name[15000]` появляется `name_rle[N]` и `#define NAME_RLE_LEN`.
Каждый массив проверяется распаковкой обратно, и без совпадения файл не трогается — картинка,
которую нечем проверить на приборе, не должна попасть в образ на слово скрипта.
"""
import re, sys, hashlib

def encode(d: bytes) -> bytes:
    out = bytearray(); i = 0; n = len(d)
    while i < n:
        j = i
        while j + 1 < n and d[j + 1] == d[i] and j - i < 127:
            j += 1
        run = j - i + 1
        if run >= 3:
            out.append(0x80 | (run - 1)); out.append(d[i]); i = j + 1
            continue
        k = i + 1
        while k < n and k - i < 128:
            r = 1
            while k + r < n and d[k + r] == d[k] and r < 3:
                r += 1
            if r >= 3:
                break
            k += 1
        out.append(k - i - 1); out += d[i:k]; i = k
    return bytes(out)

def decode(c: bytes, n: int) -> bytes:
    out = bytearray(); i = 0
    while len(out) < n:
        op = c[i]; i += 1
        cnt = (op & 0x7F) + 1
        if op & 0x80:
            out += bytes([c[i]]) * cnt; i += 1
        else:
            out += c[i:i + cnt]; i += cnt
    return bytes(out)

DECL = r"((?:static\s+)?const\s+uint8_t\s+){name}\s*\[[^\]]*\]\s*=\s*\{{(.*?)\}};"

def rows(data, per=16):
    return ",\n".join("  " + ",".join("0x%02X" % b for b in data[i:i + per])
                       for i in range(0, len(data), per))

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, names = sys.argv[1], sys.argv[2:]
    text = open(path, encoding="utf-8").read()
    if 'rle.h' not in text:
        text = text.replace("#include <stdint.h>", '#include <stdint.h>\n#include "rle.h"', 1)
    before = after = 0
    for name in names:
        m = re.search(DECL.format(name=re.escape(name)), text, re.S)
        if not m:
            sys.exit("массив %s в %s не найден" % (name, path))
        data = bytes(int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]{2}|\b\d+\b", m.group(2)))
        packed = encode(data)
        if decode(packed, len(data)) != data:
            sys.exit("%s: распаковка не совпала — файл не тронут" % name)
        digest = hashlib.sha256(data).hexdigest()[:16]
        # Хеш исходной плоскости — рядом с данными: он же стоит в тесте, и расхождение значит,
        # что картинка изменилась, а не что сжатие сломалось.
        repl = ("// %d байт после распаковки, sha256 %s\n"
                "#define %s_LEN %d\n"
                "%s%s_rle[%d] = {\n%s\n};") % (
                len(data), digest, (name + "_rle").upper(), len(packed),
                m.group(1), name, len(packed), rows(packed))
        text = text[:m.start()] + repl + text[m.end():]
        before += len(data); after += len(packed)
        print("  %-24s %6d -> %5d байт (%2.0f%% долой)  sha256 %s"
              % (name, len(data), len(packed), 100 - 100 * len(packed) / len(data), digest))
    open(path, "w", encoding="utf-8").write(text)
    print("%s: %d -> %d, экономия %.1f КБ" % (path, before, after, (before - after) / 1024))

if __name__ == "__main__":
    main()
