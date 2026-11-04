// Main dashboard renderer — ports render_states.py (§4.14) to the e-ink 1-bit path.
// Coordinates are 1:1 with the mockup; Org_01 for digits, glcd 5x7 for text.
// Grays (CO2 scale zones, graph bands) are OMITTED here — they belong to the planned
// 4-gray full-refresh layer (§4.14), a later brick. This draws the black/white structure.
#pragma once
#include <stdint.h>
#include "strip_cells.h"
#include <stdint.h>
#include "epaper.h"

struct ScreenData {
    // Header
    const char* date;      // "21.07.2026"
    const char* time;      // "19:41"
    int         batt;      // 0..100, or negative when the fuel gauge has not answered
    bool        lowbat;    // invert the % pill
    bool        wifiFail;  // "!wifi" pill after a failed sync
    bool        night;     // moon instead of clock
    bool        updClock;  // "UPD 14:05": the time of the last reading, not the current time
    bool        chg;       // charging: lightning bolt on the battery
    bool        usb;       // on the wire: plug icon in the status bar, left of the %

    // CO2 hero
    const char* co2;       // "795"
    const char* fresh;     // "4m"  -> "[4m]"
    bool        err;       // SCD41 fault: "---", hide arrow

    // Temp / Hum
    const char* temp;      // "22.8"
    const char* hum;       // "45"

    // Air quality / pressure
    const char* aq;        // "Good"
    bool        aqInvert;  // alert: invert the AQ block
    bool        humDamp;   // damp: the humidity block names it and inverts (owner, 2026-10-02)
    int         aqLevel;   // seven Bosch cells filled up to this class (0..6); -1 = none
    const char* cal;       // "3/3" (nullptr to hide)
    const char* pres;      // "1012"
    const char* presU;     // "hPa"
    int         presMode;  // 0=number, 1=Rain forecast, 2=Storm forecast (inverted)
    int         ptrend;    // pressure trend arrow: -1 down / 0 flat / +1 up (Rain/Storm modes)

    // Layout variants
    int         noNet;     // 0=normal (weather block); 2=offline full-width CO2 graph
    const char* setupLine; // bottom inverted band "smartmeteo.local (ip)"; nullptr = none
    const char* setupLine2;// second row of that band: the console password; nullptr = none

    // Graph (CO2 history)
    const float* hist;     // values, oldest..newest
    int          histN;    // count
    const char*  tStart;   // "07:41"
    const char*  tEnd;     // "19:41"
    const char*  tMid;     // "13:41" (offline full-width graph mid label)
    const char*  graphSpan;// "24H" -> graph label "CO2 [24H]"

    // Outdoor weather (nullptr city => "No weather yet")
    const char* city;      // "Darmstadt, DE"
    const char* wxFresh;   // "32m"
    int         outT;      // 13 (sign added)
    const char* mmLo;      // "+9"
    const char* mmHi;      // "+16"
    const char* desc;      // "Partly cloudy"
    const char* outHum;    // "64"
    const char* wind;      // "3"
    const char* feels;     // "13"

    // Bottom strip. WHICH three of these are shown is the owner's choice (strip_cells.h); the cells
    // stay equal at 50 px whatever goes in them, because the content is not known in advance. Every
    // field here is a finished string or nullptr for "no data" — the screen never formats, converts
    // units or rounds, exactly as it does not for anything else above.
    uint32_t    strip;     // packed cell ids, low byte = left cell; 0 means "use the default"
    const char* uv;        // "6"   clear-sky index, so an upper bound (see strip_cells.h)
    const char* sunrise;   // "07:41"; nullptr when there is no clock or no coordinates
    const char* sunset;    // "19:22"; set and cleared together with sunrise
    float       moon;      // 0 new .. 0.5 full .. <1; negative = unknown, draw nothing
    const char* wxIcon;    // OWM code "02d" (weather_icons.h)

    // The outlook under the graph. fcN 0 => no row, and the graph keeps the whole block.
    int         fcN;       // 0 or 4: tomorrow first; the row draws the first three
    const char* fcDay[4];  // "Sat 12"
    const char* fcIcon[4]; // OWM code "10d"; "" = nothing known for that day
    const char* fcHi[4];   // "22" (no plus sign; a minus prints), or "--" for a day with no data
    const char* fcLo[4];   // "13", or ""
};

void drawMainScreen(EPaper& epd, const ScreenData& d);

// Matter pairing screen (Matter mode, until the device is in a smart home). Static: drawn with a
// full refresh when its state changes, never partially.
enum PairingState { PAIR_OPEN = 0, PAIR_JOINING = 1, PAIR_CLOSED = 2, PAIR_ERROR = 3 };
struct PairingData {
    const uint8_t* qr;     // qrSize*qrSize modules, row by row, 1 = dark; nullptr = no code
    int         qrSize;
    const char* code;      // manual pairing code as printed: "1234-567-8901"
    int         state;     // PairingState
};
void drawPairingScreen(EPaper& epd, const PairingData& d);

// The two screens of switching the device off by hand. Both are static, both are drawn with a full
// refresh, and both exist for one reason: on e-ink an OFF device, a flat device and a crashed device
// look exactly alike. The glass holds its last picture for ever with no power behind it, so the
// picture has to SAY which of the three it is, or every switched-off device becomes a question.
//
// `name` is the device's own name when it has one, so a shelf with two of them is not a puzzle.
void drawOffScreen(EPaper& epd, const char* name);
void drawOffConfirmScreen(EPaper& epd, const char* name);

// The CO2 calibration, on the glass. It exists because the run has no other way to reach its owner:
// they are standing outside holding the device, the console page may be closed, the phone asleep,
// and Telegram may never have been set up — and since 2026-09-30 the run takes about a quarter of
// an hour rather than six minutes, because the device now waits for the air inside its own case to
// change before it starts counting.
//
// `trail` is the part that earns its place. During settling the numbers fall, and that fall IS what
// is being waited for: a bar would say "busy", the trail says what is happening, in the units the
// owner already reads. It is also the only way to tell afterwards whether the air had really
// settled before the six readings were taken.
struct CalibScreen {
    uint8_t     phase;      // 0 settling, 1 measuring, 2 done, 3 stopped
    int16_t     value;      // live reading in ppm; -1 when there is none to show
    int16_t     trail[6];   // readings so far, oldest first
    uint8_t     trail_n;
    uint8_t     done;       // run-up readings that have landed — the row of cells
    int16_t     corr;       // FRC correction in ppm, phase 2 only
    uint16_t    ran_min;    // minutes since the owner started it. ELAPSED, never remaining: how
                            // long the case takes cannot be known, and a countdown the device
                            // cannot keep would be worse than saying nothing.
    const char* why;        // phase 3 only: what stopped it
};
void drawCalibScreen(EPaper& epd, const CalibScreen& c);

