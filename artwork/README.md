# Artwork

Scripts that turn pictures into data the firmware draws. Their output is already committed in
`firmware/main/`; run them only when the pictures change.

| Script | Reads | Writes |
|---|---|---|
| `portal_art.py` | `portal_v4.png`, `fonts/Montserrat-Bold.ttf`, `../firmware/main/ap_password.h` | `../firmware/main/portal_art.h`, `portal_art.cpp` — the setup screen without its QR code and password, plus the glyphs to draw them |
| `rle_pack.py` | a 4-gray picture header in `../firmware/main/` | the same header, run-length compressed (format in `../firmware/main/rle.h`) |
| `logo_small.py` | `../firmware/main/logo4g.h` | `../firmware/main/logo_small.h` — the 1-bit logo on the power-off screen |
| `gen_wx_bitmaps.py` | `wxv2/` (slices of `weather_v2.png`) | `../firmware/main/weather_icons.h` — 54 px icons for the outdoor block |
| `gen_wx_small.py` | `wxv2/` | `../firmware/main/weather_icons_small.h` — 32 px icons for the forecast row |

The pictures (the weather icons, the setup screen) are under CC BY-SA 4.0; Montserrat is under the
SIL Open Font License 1.1 (`fonts/OFL.txt`).
