#pragma once
// The battery bench's screen. Everything on it comes from the log (CurveSummary::State) plus the few
// things only the moment knows (a reading taken just now, whether the cable is in). It only draws:
// no I/O, so tools/screenhost renders the same pixels on the Mac.

#include "curve_summary.h"
#include "epaper.h"

namespace CurveScreen {

struct View {
    CurveSummary::Next next = CurveSummary::Next::Empty;
    float    v_now   = 0;        // a reading taken just now, with the gain; 0 = the log's last
    float    end_v   = 3.20f;    // where a discharge ends (rested, with the gain)
    float    gain    = 1.0f;
    bool     calibrated = false;
    bool     usb     = false;
    uint32_t now_abs = 0;        // the clock now, for the elapsed times
    int      next_load_min = 15; // the next load phase, for the discharge view
    const char *alert = nullptr; // the bottom bar, when the moment says more than the log (else from `next`)
    const char *page = nullptr;  // the charge page's address, while it is up ("http://192.168.1.23/")
    int      pin = 0;            // the code the page asks for, shown only here
};

void draw(EPaper &e, const CurveSummary::State &s, const View &v);

}  // namespace CurveScreen
