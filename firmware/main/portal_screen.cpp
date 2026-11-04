#include "portal_screen.h"
#include "portal_art.h"
#include "qrcodegen.h"
#include "rle.h"
#include "org01.h"
#include "battery_icons.h"
#include <cstdio>
#include <cstring>

namespace PortalScreen {
namespace {

constexpr int kW = 400, kH = 300, kRow = kW / 8;

// A pixel's gray level, 0 black .. 3 white, in the planes of Good Display's Display_4Gray table:
// black 0/0, dark gray 1/0, light gray 0/1, white 1/1 (RAM 0x24 / 0x26) — so level = b24 | b26 << 1.
// Darker wins: the anti-aliased edge of a glyph never lightens ink that is already there.
void darken(uint8_t *p24, uint8_t *p26, int x, int y, uint8_t level)
{
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    const size_t i = (size_t)y * kRow + (size_t)(x / 8);
    const uint8_t bit = (uint8_t)(0x80 >> (x % 8));
    const uint8_t cur = (uint8_t)(((p24[i] & bit) ? 1 : 0) | ((p26[i] & bit) ? 2 : 0));
    if (level >= cur) return;
    if (level & 1) p24[i] |= bit; else p24[i] &= (uint8_t)~bit;
    if (level & 2) p26[i] |= bit; else p26[i] &= (uint8_t)~bit;
}

// One cell of portal_glyph_px: w columns x PORTAL_LINE_H rows, 2 bits a pixel, MSB first.
void blit(uint8_t *p24, uint8_t *p26, uint16_t off, int w, int x0)
{
    for (int y = 0; y < PORTAL_LINE_H; y++)
        for (int x = 0; x < w; x++) {
            const size_t k = (size_t)y * (size_t)w + (size_t)x;
            const uint8_t v = (uint8_t)((portal_glyph_px[off + k / 4] >> (6 - 2 * (k % 4))) & 3);
            if (v != 3) darken(p24, p26, x0 + x, PORTAL_LINE_TOP + y, v);
        }
}

const portal_glyph_t *glyph(char c)
{
    for (const portal_glyph_t &g : portal_glyphs)
        if (g.ch == c) return &g;
    return nullptr;
}

// The "WIFI:" text both phone platforms join from (the ZXing form): backslash, semicolon, comma,
// colon and double quote are escaped in the name and the password.
bool append_escaped(char *out, size_t cap, size_t &len, const char *s)
{
    for (; *s; s++) {
        if (strchr("\\;,:\"", *s)) {
            if (len + 1 >= cap) return false;
            out[len++] = '\\';
        }
        if (len + 1 >= cap) return false;
        out[len++] = *s;
    }
    out[len] = '\0';
    return true;
}

bool wifi_qr_text(char *out, size_t cap, const char *ssid, const char *password)
{
    size_t len = 0;
    out[0] = '\0';
    auto lit = [&](const char *s) {
        const size_t n = strlen(s);
        if (len + n >= cap) return false;
        memcpy(out + len, s, n + 1);
        len += n;
        return true;
    };
    return lit("WIFI:S:") && append_escaped(out, cap, len, ssid) && lit(";T:WPA;P:") &&
           append_escaped(out, cap, len, password) && lit(";;");
}

// Unconditional writes for the battery group: it is pixel art in black and white over a white
// corner, and the bolt is WHITE over its own black halo, which darken() could never do.
void put(uint8_t *p24, uint8_t *p26, int x, int y, uint8_t level)
{
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    const size_t i = (size_t)y * kRow + (size_t)(x / 8);
    const uint8_t bit = (uint8_t)(0x80 >> (x % 8));
    if (level & 1) p24[i] |= bit; else p24[i] &= (uint8_t)~bit;
    if (level & 2) p26[i] |= bit; else p26[i] &= (uint8_t)~bit;
}

void fill(uint8_t *p24, uint8_t *p26, int x, int y, int w, int h, uint8_t level)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) put(p24, p26, xx, yy, level);
}

// Org_01 the way EPaper::drawGFXChar() draws it: y is the baseline, every set bit a size x size block.
int org_width(const char *t, int size)
{
    int w = 0;
    for (; *t; t++) {
        const uint8_t c = (uint8_t)*t;
        w += (c < Org_01.first || c > Org_01.last) ? 6 * size
                                                   : Org_01.glyph[c - Org_01.first].xAdvance * size;
    }
    return w;
}

void org(uint8_t *p24, uint8_t *p26, int x, int y, int size, const char *t, uint8_t level)
{
    for (; *t; t++) {
        const uint8_t c = (uint8_t)*t;
        if (c < Org_01.first || c > Org_01.last) { x += 6 * size; continue; }
        const GFXglyph &g = Org_01.glyph[c - Org_01.first];
        uint16_t bo = g.bitmapOffset;
        uint8_t bits = 0, bit = 0;
        for (int yy = 0; yy < g.height; yy++)
            for (int xx = 0; xx < g.width; xx++) {
                if (!(bit & 7)) bits = Org_01.bitmap[bo++];
                bit++;
                if (bits & 0x80)
                    fill(p24, p26, x + (g.xOffset + xx) * size, y + (g.yOffset + yy) * size, size, size, level);
                bits <<= 1;
            }
        x += g.xAdvance * size;
    }
}

} // namespace

void drawBattery(uint8_t *p24, uint8_t *p26, const Battery &b)
{
    constexpr uint8_t INK = 0, PAPER = 3;
    // The dashboard header's numbers (screen.cpp, drawMainScreen): outline 27 x 14 at (362, 8) with a
    // 3 x 6 nub, fill up to 22 px, percent in Org_01 size 2 on baseline 19 ending 8 px left of the
    // outline, the plug 8 px left of the percent. tests/t_portal.cpp compares the two corners pixel
    // for pixel, so a change on one screen that is not made on the other fails there.
    const int bx = 362;
    const bool known = b.percent >= 0;
    const int pct = b.percent > 100 ? 100 : b.percent;
    const bool low = known && pct < 15 && !b.usb;

    int left = bx;                       // where the group's next element must end, minus the gap
    if (known) {
        char bs[8];
        snprintf(bs, sizeof(bs), "%d", pct);
        const int numw = org_width(bs, 2), pw = numw + 4 + org_width("%", 2), sx = bx - 8 - pw;
        if (low) fill(p24, p26, sx - 5, 9, pw + 9, 14, INK);
        const uint8_t c = low ? PAPER : INK;
        org(p24, p26, sx, 19, 2, bs, c);
        org(p24, p26, sx + numw + 4, 19, 2, "%", c);
        left = sx - (low ? 5 : 0);
    }
    // With no figure the plug keeps the same 8 px gap, to the outline instead of the digits.
    if (b.usb)
        for (int r = 0; r < 12; r++)
            for (int c = 0; c < 10; c++)
                if (PLUG_ICON[r][c] == 'X') put(p24, p26, left - 18 + c, 9 + r, INK);

    fill(p24, p26, bx, 8, 27, 1, INK);  fill(p24, p26, bx, 21, 27, 1, INK);
    fill(p24, p26, bx, 8, 1, 14, INK);  fill(p24, p26, bx + 26, 8, 1, 14, INK);
    fill(p24, p26, bx + 27, 12, 3, 6, INK);
    if (known) {
        int lvl = (pct * 22 + 50) / 100;
        if (lvl < 1) lvl = 1;
        fill(p24, p26, bx + 2, 10, lvl, 10, INK);
    }
    if (b.charging) {
        const int x0 = bx + 13 - 5, y0 = 15 - 7;
        for (int r = 0; r < 14; r++)
            for (int c = 0; c < 10; c++)
                if (BOLT_V1[r][c] == 'X')
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) put(p24, p26, x0 + c + dx, y0 + r + dy, INK);
        for (int r = 0; r < 14; r++)
            for (int c = 0; c < 10; c++)
                if (BOLT_V1[r][c] == 'X') put(p24, p26, x0 + c, y0 + r, PAPER);
    }
}

bool expandArt(uint8_t *p24, uint8_t *p26)
{
    // The picture ships run-length coded; there is no plain copy in flash to hand out a pointer to,
    // which is why this fills the caller's planes instead of returning one (it used to be art24()).
    return rle_expand(p24, kPlaneBytes, portal_art_plane24_rle, PORTAL_ART_PLANE24_RLE_LEN)
        && rle_expand(p26, kPlaneBytes, portal_art_plane26_rle, PORTAL_ART_PLANE26_RLE_LEN);
}

bool compose(uint8_t *p24, uint8_t *p26, const char *ssid, const char *password)
{
    if (!expandArt(p24, p26)) return false;

    // Everything is checked before anything is drawn, so a refusal leaves the bare picture.
    const size_t n = strlen(password);
    const portal_glyph_t *gs[32];
    if (n == 0 || n > sizeof(gs) / sizeof(gs[0])) return false;
    for (size_t i = 0; i < n; i++)
        if (!(gs[i] = glyph(password[i]))) return false;

    char text[128];
    if (!wifi_qr_text(text, sizeof(text), ssid, password)) return false;
    // The frame in the picture holds exactly this version; the lowest error correction is the floor
    // and qrcodegen raises it while the text still fits (medium, for the device's own name and password).
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(PORTAL_QR_VERSION)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(PORTAL_QR_VERSION)];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, PORTAL_QR_VERSION, PORTAL_QR_VERSION,
                              qrcodegen_Mask_AUTO, true))
        return false;

    const int size = qrcodegen_getSize(qr);
    for (int my = 0; my < size; my++)
        for (int mx = 0; mx < size; mx++)
            if (qrcodegen_getModule(qr, mx, my))
                for (int dy = 0; dy < PORTAL_QR_MODULE; dy++)
                    for (int dx = 0; dx < PORTAL_QR_MODULE; dx++)
                        darken(p24, p26, PORTAL_QR_X + mx * PORTAL_QR_MODULE + dx,
                               PORTAL_QR_Y + my * PORTAL_QR_MODULE + dy, 0);

    // "Password: <value>", centred on the line's ink as the approved picture is.
    int32_t adv = PORTAL_LABEL_ADV;
    for (size_t i = 0; i + 1 < n; i++) adv += gs[i]->adv;
    // Pen positions are rounded to the nearest pixel (the cells were drawn on whole pixels), which keeps
    // the spacing error within half a pixel either way instead of up to one pixel to the left.
    const auto px = [](int32_t sub) { return (int)((sub + PORTAL_SUBPX / 2) / PORTAL_SUBPX); };
    const int total = px(adv) - PORTAL_GLYPH_PAD + gs[n - 1]->ink_r + 1;
    const int x0 = PORTAL_LINE_CX - total / 2;

    blit(p24, p26, PORTAL_LABEL_OFF, PORTAL_LABEL_W, x0);
    int32_t pen = (int32_t)x0 * PORTAL_SUBPX + PORTAL_LABEL_ADV;
    for (size_t i = 0; i < n; i++) {
        blit(p24, p26, gs[i]->off, gs[i]->w, px(pen) - PORTAL_GLYPH_PAD);
        pen += gs[i]->adv;
    }
    return true;
}

} // namespace PortalScreen
