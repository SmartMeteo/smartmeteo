// Byte run-length coding for the four-gray pictures, and nothing else.
//
// The logo, the LOW BATTERY lock and the Wi-Fi setup picture are 400x300 two-plane bitmaps: 15 000
// bytes each, six of them, 90 KB of an image with 2 % of its slot left. They are flat artwork, so
// they run-length code down to about a quarter of that — 66 KB back for forty lines of code.
//
// Why not gzip, which gets 82 KB: there is no inflate on this chip. ESP-IDF ships none for the C6
// and the ROM has none, so it would have to be linked in — and an inflate costs more than the
// twelve kilobytes the better ratio wins. This codes and decodes in a few hundred bytes.
//
// Format, one control byte at a time:
//   0x80 | (n-1)   repeat the NEXT byte n times          (n = 1..128)
//   (n-1)          copy the next n bytes verbatim        (n = 1..128)
// The encoder is Software/mockups/rle_pack.py, which round-trips every array it writes.
#pragma once

#include <cstddef>
#include <cstdint>

// Expands `src` into `dst`. Returns false when the stream would run past either end, which means
// the two sides disagree — a truncated array or the wrong length — rather than a corrupt picture.
static inline bool rle_expand(uint8_t *dst, size_t dst_len, const uint8_t *src, size_t src_len)
{
    size_t o = 0, i = 0;
    while (o < dst_len) {
        if (i >= src_len) return false;
        const uint8_t op = src[i++];
        const size_t n = (size_t)(op & 0x7F) + 1;
        if (o + n > dst_len) return false;
        if (op & 0x80) {
            if (i >= src_len) return false;
            const uint8_t v = src[i++];
            for (size_t k = 0; k < n; k++) dst[o++] = v;
        } else {
            if (i + n > src_len) return false;
            for (size_t k = 0; k < n; k++) dst[o++] = src[i++];
        }
    }
    return i == src_len;   // the whole stream was used: a short read means the wrong array
}
