#pragma once
// Consumption, measured rather than modelled: the slope of the charge curve while running on the
// battery, read from the history ring. This is the only number on the battery card that cannot be
// obtained from a datasheet — it answers "what does this device actually cost in the mode it is in".
//
// ONE estimator for the whole card. Until 2026-10-03 the card had two: this one ("Measured draw",
// "Life at that draw") and a second slope kept by BatteryMonitor in RTC_DATA ("Left at this rate",
// "Discharge rate", "Est. daily use"). The second one was wiped by every restart — every update
// installed over the air — and spoke after six hours whatever the gauge had moved, so it put
// "523 days, 0.16 %/day, 3 mAh" on a device whose own measured budget is ~13 mAh a day, under a
// notice from this one saying "Collecting". The history ring survives restarts; the RTC slope did
// not. So the card asks this, and only this.
//
// Pure: no ESP-IDF, no flash. The caller reads the history; this decides. tests/t_cycle.cpp feeds
// it synthetic runs.

#include <stdint.h>

namespace PowerEstimate {

struct Point { uint32_t t; float soc; float v; bool usb; };

struct Result {
    float ua      = 0;    // average current
    float pct_day = 0;    // the same slope in % of the charge per day
    float hours   = 0;    // span the slope was fitted over: the run, past the settling
    float drop    = 0;    // charge the FIT says went over that span, % — the line, not two samples
    float mv      = 0;    // and the voltage the cell actually lost over it
    float months  = 0;    // projected runtime on a full cell
    int   conf    = 0;    // 0 = too early to say, 1 = rough, 2 = trustworthy
    int   dropped = 0;    // points left out as outliers (see OUTLIER_SIGMA)
    bool  valid   = false;
};

// Points oldest first. Two things make a naive slope lie, and each is handled inside:
//   * charging — only the newest unbroken run of battery-powered points is used;
//   * relaxation — a cell just off the charger sheds volts for almost no charge, so the first
//     SETTLE_S of a run that begins at a cable are dropped.
// Everything after that is ONE least-squares line over the whole remaining run, with a second pass
// that leaves out points sitting further than OUTLIER_SIGMA from the first. There is no filter for
// "steps" any more and that is the point — see the long note in fit().
Result fit(const Point *pts, int n, uint32_t cap_mah);

// How long after a cable the cell is still settling. Until 2026-10-08 this was a VOLTAGE — points
// above 4.05 V were dropped — chosen on the 804050, where 4.05 V is ~98 % and the rule cost an hour.
// On the v1 board's 103450 the same 4.05 V is ~87 %: the rule threw away the first eight days of
// every charge, and the field board had no estimate a week after it left the charger. Relaxation is
// a matter of hours on any lithium cell; time is the honest unit for it.
constexpr uint32_t SETTLE_S = 12 * 3600;

// How far from the line a point has to sit to be left out of the second pass. Standard deviations of
// the first pass's own residuals, so it adapts to how noisy this particular run is instead of naming
// a percentage — and it is SYMMETRIC: a point is judged by its distance from the line, never by which
// side of it. That symmetry is the whole correction of 2026-10-19 and the reason three constants
// (JUMP_RISE_PCT 0.5, JUMP_FALL_PCT 1.0, JUMP_FALL_PCT_H 6.0) are gone rather than retuned.
//
// They subtracted every rise over half a percent as "the estimate re-anchoring" and kept the falls.
// That was true while percent() held a no-rise latch in RTC_DATA that every restart re-initialised;
// the latch was removed on 2026-10-14 and the filter was not. On this cell's plateau one millivolt
// is 0.83 % and the gauge is good to +-7.5 mV, so plain noise clears half a percent several times a
// day — and a one-sided cut turns noise into drift. Measured on the field board's own month, through
// this file: 2.05 %/day over ten days and 1.41 over twenty-six, against 0.65-0.71 from the voltage,
// the event-log budget and the cell curve; its own log admitted removing +8.06 % and +14.93 % of
// "steps" from a cell that had really given up 25 points.
//
// Three sigma, because a least-squares line is what noise is supposed to be averaged by: at three
// sigma a clean run loses about one point in 400 (nothing), while a console session or a restart —
// the real steps, and they are rare — stands many sigma out and goes.
constexpr float OUTLIER_SIGMA = 3.0f;

// A day at a few hundred microamps moves the charge less than the gauge's own accuracy, so below
// this much drop the direction is real and the number is not. MEASURED OVER THE WINDOW THE SLOPE WAS
// FITTED OVER, which until 2026-10-13 it was not: drop and hours described the whole run while the
// number came from its newer half, so the badge vouched for a span it had never looked at. That is
// how a division by nothing came to be labelled "enough to trust" — the field board's percentage
// stood still for sixty hours, the fit returned rounding dust, and the whole-run drop of 3.1 %
// certified it (journal 2026-10-13).
//
constexpr float MIN_DROP_PCT  = 1.0f;
constexpr float GOOD_DROP_PCT = 3.0f;

// And a floor far below either, for a window that did not move AT ALL. A least-squares line through
// a constant is flat but not exactly flat — it comes back at 1e-13 %/day — and the only guard was
// "reject a slope above zero", which that passes with room to spare; dividing by it put 2^31 days on
// the owner's dashboard (journal 2026-10-13). A tenth of a percent is below every mode this device
// has and nine orders of magnitude above that dust, so it separates "slow" from "nothing" without
// having an opinion about slow.
//
// This is deliberately NOT the gate on the card's three facts: those wait for conf >= 1, i.e. a
// whole percent across the fitted window, and that is what bounds them. A hundred percent divided
// by at-least-one-percent-per-window cannot exceed a hundred windows, so the longest runtime the
// card can state is about 500 days — by construction, not by a cap. Anything beyond is arithmetic
// that has gone wrong, and this floor and MIN_DROP_PCT are where it stops.
constexpr float MIN_FIT_PCT = 0.1f;

// How far back the estimate looks, and how many points it keeps. A time, not a count of records:
// 600 records were six days at the default 15-minute step and two at 5 minutes, which on a slow cell
// is not enough to see anything move. Ten days, ALL of which the slope now uses — until 2026-10-19
// it used the newer half, which on a cell this slow was five days, and five days at 0.65 %/day is
// 3.3 % against a gauge accurate to 6: the window was shorter than the thing it measured. The half
// was there to keep the relaxation out and SETTLE_S already does that, by time. Ten days remains the
// span because that is what a mode change takes to be fully reflected, and because it keeps the
// answer about the mode the device is in now. The points are thinned to MAX_POINTS on the way in
// (power_history.cpp), never dropping a cable record.
constexpr uint32_t WINDOW_S   = 10 * 86400;
constexpr int      MAX_POINTS = 600;

// What the card's first three facts show. Withheld — zeros and -1, which the page draws as a dash —
// unless the fit has one (conf >= 1, i.e. MIN_DROP_PCT moved across the fitted window). These three,
// unlike "Measured draw", have no line beside them to say how sure the device is, so they say
// nothing rather than something unqualified. Time to EMPTY is withheld on the cable as well, where
// nothing is emptying.
struct Card { float pct_day; float mah_day; float tte_h; };
Card card(const Result &r, float soc_now, bool on_usb);

}  // namespace PowerEstimate
