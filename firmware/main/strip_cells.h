#pragma once
// What the three cells in the bottom-right of the screen may hold, and how that choice is stored.
//
// This is its own header for the same reason ap_password.h is: the ids are read by the screen, by
// the web API that validates them, and by the console that offers them, and none of those should
// have to include either of the others to agree on a number.
//
// THE IDS ARE A WIRE FORMAT. They are packed one per byte into NVS `strip` (low byte = left cell)
// and the console sends the same packed word back. Renumbering one silently rearranges the screen
// of every device already in the field. Append only.
//
// The cells are EQUAL, 50 px, and stay equal whatever goes in them: the owner picks the content, so
// the layout cannot be fitted to it. Everything below therefore has to fit a 50 px cell at the ink
// budget of 44 px the strip already uses. Measured worst cases, in ink px:
//
//     content          worst      s1   s2   s3    largest size that fits
//     clock "HH:MM"    00:00      25   50   75    1     <-- the only outlier
//     Hum/Rain/Cloud   100        13   26   39    3
//     Feels            125        13   26   39    3
//     High/Low         -40        16   32   48    2
//     Dew              -12        12   24   36    3
//     Wind, UV         12 / 11     7   14   21    3
//
// A clock is five characters where every other reading is two or three, and by the strip's own rule
// — one size per row, the largest that fits them all — a single clock would drag its neighbours
// down by a factor of six in area. That is why SUN holds BOTH times in one cell, two rows, size 1,
// with the mark to the right: 37 x 16 px inside 50 x 37 at the worst case 00:00 / 20:00.
//
// Never reason about these widths from a sample. The Org digits are not monospaced — '1' and ':'
// advance 4 px at size 2 where every other digit takes 12 — so "07:41" measures a comfortable 42 px
// and "00:00" a fatal 50. The same trap makes "-12" (36 px at size 3) safe and "-40" (48 px) not,
// which is why the row size follows the DATA and can change through the day.
//
// Availability is not uniform and the console must say so per option, not disable the card:
//   MOON  needs only the date. Always available, in every mode.
//   SUN   needs coordinates (NVS wx_lat/wx_lon) and a clock. No network.
//   rest  come from the weather, which Matter, no_wifi and Ultra-Save-on-plain-Wi-Fi never fetch.
// In those three modes the strip today is three sets of dashes, so the two offline-capable cells
// are exactly what makes it worth having.
//
// UV is MET's ultraviolet_index_clear_sky: the index for a clear sky, an upper bound rather than a
// measurement. Under cloud the real one is lower. Safe direction for "do not get burnt", but the
// console must word it as "no worse than this".
//
// Dropped by the owner 2026-09-24: wind gust (wind alone is enough), and single Sunrise / single
// Sunset cells (the pair costs the same one cell and cannot poison the row size).
//
// Dropped by the owner 2026-09-28: cloud cover, dew point, today's high and today's low. Seven
// options is a list somebody reads; eleven is a list somebody scrolls past. The four that went are
// the ones a hub is least likely to supply, so they were also the likeliest to be dashes for ever.
// Rain chance went the same day for a harder reason: the data does not exist outside the Nordics.
// Their numbers are retired rather than reused — see the enum.
//
// The six that remain were checked against LIVE data before this was settled, not assumed. At the
// device's own location MET returns apparent_air_temperature, relative_humidity, wind_speed and
// ultraviolet_index_clear_sky; the hub path delivers the same four plus the outlook. Sunrise,
// sunset and the moon need no network at all.
#include <stdint.h>

enum StripCell : uint8_t {
    STRIP_SUN   = 0,   // sunrise and sunset, both in this one cell
    STRIP_MOON  = 1,
    STRIP_HUM   = 2,
    STRIP_WIND  = 3,
    STRIP_FEELS = 4,
    STRIP_UV    = 5,
    // 6..10 are retired. 6 was Rain chance, dropped 2026-09-28 once MET was found to publish
    // probability_of_precipitation only inside the Nordics: for Darmstadt, London and everywhere
    // else its forecast carries precipitation_amount and no probability at all, so the cell could
    // never fill. Verified live against api.met.no — Oslo and Bergen have it, Darmstadt and London
    // do not — and the same hole reaches a hub whose weather integration is Met.no.
    // 7..10 were Cloud cover, Dew point, Today's high and Today's low, dropped the same day.
    // The numbers are RETIRED, not free: a device in the field may still hold one in NVS, and a new
    // option reusing a number would quietly take over somebody else's cell. The screen already
    // substitutes the default for an id it does not know, so a retired one costs nothing.
    STRIP_OPT_COUNT
};

// Humidity / Wind / Feels — what the screen has always shown. A device that has never been told
// otherwise must keep exactly the strip its owner already knows.
static constexpr uint32_t STRIP_DEFAULT =
    (uint32_t)STRIP_HUM | ((uint32_t)STRIP_WIND << 8) | ((uint32_t)STRIP_FEELS << 16);

static inline uint8_t strip_cell(uint32_t packed, int i) {
    return (uint8_t)((packed >> (8 * i)) & 0xFF);
}
