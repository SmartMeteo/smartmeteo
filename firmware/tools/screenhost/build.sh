#!/usr/bin/env bash
# Renders the device screen on this computer, from the firmware's OWN code.
#
# screen.cpp and epaper.cpp are compiled here as they are — same glyphs, same width arithmetic, same
# layout decisions — with ESP-IDF replaced by the stubs beside this file. That is the whole point:
# Software/mockups/ holds Python re-drawings of the same screen, and a re-drawing tells you about
# itself, not about the firmware. This tells you what the glass will show.
#
#     tools/screenhost/build.sh && ./screenhost extreme && open out_extreme.pgm
#
# States: normal, extreme, band, mmhg, empty, night, charge, offline. Add one in main.cpp.
#
# -Dprivate=public is a test-only trick to read the frame buffer; nothing in the firmware changes.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$HERE/../../main"
c++ -std=gnu++17 -Dprivate=public -I"$HERE/stubs" -I"$MAIN" -o "$HERE/screenhost" \
    "$HERE/main.cpp" "$MAIN/screen.cpp" "$MAIN/epaper.cpp" "$MAIN/epd_vendor.cpp"
echo "собрано: $HERE/screenhost"
