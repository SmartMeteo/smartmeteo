// Adafruit-GFX-compatible font structs, standalone (no Adafruit_GFX dependency).
// Lets us reuse the vendored Org_01 pixel font for large digits on the e-ink.
#pragma once
#include <stdint.h>

typedef struct {
    uint16_t bitmapOffset;   // pointer into GFXfont->bitmap
    uint8_t  width, height;   // bitmap dimensions in pixels
    uint8_t  xAdvance;        // distance to advance cursor (x axis)
    int8_t   xOffset, yOffset;// dist from cursor pos to UL corner
} GFXglyph;

typedef struct {
    uint8_t  *bitmap;         // glyph bitmaps, concatenated
    GFXglyph *glyph;          // glyph array
    uint16_t  first, last;    // ASCII extents
    uint8_t   yAdvance;       // newline distance (y axis)
} GFXfont;
