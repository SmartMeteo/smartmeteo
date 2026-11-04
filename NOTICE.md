# Third-party code, fonts and data

Smart Meteo's own code is under the Apache License 2.0 ([`LICENSE`](LICENSE)), its hardware under
CERN-OHL-P-2.0 and its documentation under CC BY-SA 4.0 (both in [`LICENSES/`](LICENSES/)). The
parts below come from others and keep their own terms.

## In this repository

| What | Where | Licence |
|---|---|---|
| ArduinoJson 7.4.3, Benoît Blanchon | `firmware/components/ArduinoJson/` | MIT — `firmware/components/ArduinoJson/LICENSE.txt` |
| QR Code generator library, Project Nayuki (copied from connectedhomeip's `examples/common/QRCode`) | `firmware/main/qrcodegen.c`, `qrcodegen.h` | MIT — in the file headers |
| BME69x SensorAPI 1.1.0, Bosch Sensortec | `firmware/main/bme69x.c`, `bme69x.h`, `bme69x_defs.h` | BSD-3-Clause — `LICENSES/BSD-3-Clause-Bosch-SensorAPI.txt` |
| Adafruit GFX Library: the classic 5×7 font and the font structures | `firmware/main/glcdfont.c`, `gfxfont.h` | BSD — `LICENSES/BSD-2-Clause-Adafruit-GFX.txt` |
| Org_01 pixel font, Orgdot (www.orgdot.com/aliasfonts), as distributed with Adafruit GFX | `firmware/main/org01.h` | free font shipped with Adafruit GFX — same file |
| Montserrat Bold, Julieta Ulanovsky and the Montserrat Project Authors; its glyphs are pre-rendered into the setup screen | `artwork/fonts/Montserrat-Bold.ttf`, rendered into `firmware/main/portal_art.cpp` | SIL Open Font License 1.1 — `artwork/fonts/OFL.txt` |
| Good Display GDEY042T81 driver sequences: command bytes, arguments, ordering and delays follow Good Display's Arduino demo for this panel and the SSD1683 datasheet | `firmware/main/epd_vendor.cpp` | vendor reference code |

The weather icons were drawn by the author for Smart Meteo; the logo and the setup-screen
picture were made for the project too. They are under the project's own licences: as firmware
data under Apache-2.0, as pictures in `artwork/` under CC BY-SA 4.0. The name and the logo as a
mark are not licensed (see README).

## Fetched at build time, not in this repository

| What | Licence |
|---|---|
| ESP-IDF, esp-matter, connectedhomeip (Matter SDK), OpenThread, Espressif managed components (`firmware/dependencies.lock`) | Apache-2.0 (OpenThread: BSD-3-Clause) |

## Not included

**Bosch BSEC** (the air-quality index library, its headers and its configuration) is not ours to
redistribute. The firmware builds without it; see `firmware/components/bsec2/README.md`.

## Data services

| Service | Used for | Terms |
|---|---|---|
| MET Norway Locationforecast | weather and forecast when the device fetches its own | CC BY 4.0 — attribution is shown in the console |
| OpenWeatherMap | weather, only with the owner's own key | the owner's agreement with OpenWeatherMap |
| GeoNames `cities500` | city search, `firmware/tools/vps/geo/` | CC BY 4.0 |
| Open-Meteo geocoding | fallback city search | Open-Meteo terms |
