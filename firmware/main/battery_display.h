#pragma once
// The percentage the OWNER sees, kept apart from the one the cell is measured at.
//
// Why there are two (decision of the owner, 2026-10-05). Unplugging a full device used to show
// "100 %" and, minutes later, 85: the measured figure was right, the jump was what read as broken.
// A phone does not do that, and the reason is not that its cell is better — it is that the screen
// shows a number derived from the measurement, not the measurement:
//
//   * 100 % means "the charger finished". Some hours after a finished charge, once the cell has
//     settled, the device notes what the curve calls that state (94 %, say) and from then on shows
//     the measured figure stretched by 100/94. Full is 100, the lockout is 0, and in between the
//     number falls evenly. The factor is learned again at every finished charge, so a different
//     cell, a different batch and an ageing one are all absorbed without a new table.
//   * On the battery the number never JUMPS: it moves toward the stretched measurement at 0.5 point
//     an hour, the pace the owner chose — twenty points in about two days. Since 2026-10-19 that is
//     a rate LIMIT and not a one-way catch-up, so the figure can also creep back up when the
//     measurement does. It was a latch until then, and a latch is not the same promise: the number
//     could only ever take the lowest value the target had ever reached, and ONE loaded reading
//     stayed there for good — measured on the field board, 67 % on the glass against the 74.5 %
//     its own volts give through its own cell's table, for a month. At half a point an hour the
//     whole number can still gain at most one digit in two hours, and only after the measurement
//     itself has risen, so "it does not climb on the battery" remains true to a reader; what it can
//     no longer do is sit below the cell for ever. See step() for the measurement and the mechanism.
//   * Below 20 % the stretch fades out and the pace is 2 points an hour, so the low-battery
//     messages (15 / 10 / 5 %) and the lockout land on the same number the glass shows.
//
// What does NOT use this: the lockout (it compares volts), the history ring (a measurement record),
// the consumption estimate (it fits the history). Only the screen, the console, Telegram and the hub
// read the shown figure. A figure tuned for a human never decides anything about the cell.
//
// Pure: no ESP-IDF. BatteryMonitor keeps the state in RTC memory and the factor in NVS; this file
// only decides. tests/t_cycle.cpp drives it through whole charge cycles.

#include <stdint.h>

namespace BatteryDisplay {

// The owner's pace, points per hour — and since 2026-10-19 a LIMIT, not a one-way catch-up: the
// figure moves toward the measurement at most this fast, down or up. The latch it replaces let one
// loaded reading (a console session: tens of minutes of radio, tens of millivolts of sag) become a
// permanent ceiling, and on the field board that cost seven and a half points for a month. See the
// long note in step(). Half a point an hour is eighteen times the cell's real decline, so the glass
// still follows it; and it is slow enough that the whole number can gain at most one digit in two
// hours, so "it does not climb on battery" stays true to a reader.
// How long the aim is averaged over before the pace is applied to it. A rate limiter cannot follow
// a signal that jitters by more than one of its steps — it moves up a step, down a step, and tracks
// nothing — so the jitter has to go first.
//
// MEASURED, not assumed (field board, 604 records over 339 hours, read back out of Home Assistant
// 2026-10-19). Between neighbouring readings the terminal voltage moves by a median of 2.0 mV, 3.0
// at the ninetieth percentile, 15 at the ninety-ninth and 30 mV at worst; against the hourly mean
// it sits 1.2 mV out typically and 16.5 mV at worst. On this cell's plateau a millivolt is 0.83 %,
// so that is a point and a half of jitter in the ordinary case and up to TWENTY-FIVE in a single
// reading — forty times the limiter's own step, which is why nothing can be filtered by the limiter
// alone and why the latch this replaces could be dragged seven points down by one bad sample.
//
// Four hours is chosen against those numbers: a lone 25-point excursion moves the aim by about one
// and a half points before it decays, and the ordinary jitter comes out as a ripple of hundredths.
// An hour would let that same excursion through at six points. The cost is a lag of tau times the
// real decline — a tenth of a point at 0.68 %/day, a third at 2 %/day — which is below the
// resolution of the figure the owner reads. Not applied below LOW_PCT; see step().
constexpr double SMOOTH_TAU_S      = 4 * 3600.0;
constexpr float  CATCHUP_PCT_H     = 0.5f;
constexpr float  CATCHUP_LOW_PCT_H = 2.0f;          // below LOW_PCT: agree with the measurement soon
                                                    // (the lockout and the warnings read the measurement)
constexpr float  LOW_PCT           = 20.0f;         // below this the stretch is gone entirely
constexpr float  FADE_TOP_PCT      = 30.0f;         // ...and between the two it fades out
constexpr double SETTLE_S          = 4.0 * 3600.0;  // a finished charge is learned this long after
constexpr float  K_MAX             = 1.25f;         // a "full" cell reading below 80 % is not learned
constexpr float  MIN_FULL_PCT      = 100.0f / K_MAX;
constexpr double MAX_STEP_S        = 6.0 * 3600.0;  // a longer gap is a clock step and credits nothing

// RTC-resident. `magic` is what tells a state this code wrote from bytes a previous image left:
// anything else resets it, and a reset only costs the glide from one jump.
struct State {
    uint32_t magic;
    float    shown;
    float    tgt_f;         // the filtered aim: the measurement with its jitter taken out          // -1: nothing shown yet
    double   last_t;         // wall seconds of the last step
    double   unplug_t;       // when a finished charge came off the cable
    uint8_t  on_usb;         // the last step was on the cable
    uint8_t  full_seen;      // the charger finished during this stay on the cable
    uint8_t  learn_pending;  // a finished charge is settling and will be learned
};
constexpr uint32_t MAGIC = 0xB15D0007u;   // 0005 held prev_target; 0006 had no filtered aim

void reset(State &s);

// The measurement stretched by k, with the stretch fading out between FADE_TOP_PCT and LOW_PCT.
// Monotonic in `measured`, so a falling measurement can never make it rise.
float target(float measured, float k);

struct Step {
    float shown;
    bool  learned;     // a finished charge was learned on this step...
    float k;           // ...and this is the factor to keep (equal to the one passed in otherwise)
};

// One reading. `measured` is the cell (0..100), `cable_shown` what the charging logic shows while
// the cable is in (ignored on the battery), `done` the charger's own "finished". `k` and `have_k`
// are the factor held in storage and whether one was ever learned: the first one is taken as
// measured, later ones move halfway, so a single odd charge cannot swing the scale.
Step step(State &s, float measured, bool on_usb, bool done, float cable_shown,
          double now_s, float k, bool have_k);

}  // namespace BatteryDisplay
