#pragma once
// How THIS board reads its cell, in one place, because three callers need the answer and none of
// them should have to know which board they are running on: the lockout in bench_screens.cpp, the
// measurement path, and parking before sleep. One image serves both revisions — the board is
// identified at run time, not by a build flag, so a v1 and a v2 device take the same OTA.
//
// v1 carries a MAX17048 fuel gauge at 0x36. v2 does not: the gauge was dropped for cost, and the
// cell is read through a resistor divider instead, with the charger's own STDBY# line telling us
// when the charge is finished.
//
// Measured on the first assembled v2 board, 2026-09-29: divider output 1153 mV for a cell at
// 4.11 V, which is the ratio below to three digits.

#include <stdint.h>
#include "driver/i2c_master.h"

namespace BatterySource {

// R23 1 MOhm from BATT+ to the node, R12 390 kOhm from the node to ground (schematic v2,
// 2026-09-29). The divider sits BEFORE the power switch, on the raw cell, so it draws its 3.0 uA
// around the clock; that was accepted on 2026-09-21 as about 1 % of the sleep floor, because
// removing it needs a low-side switch and a GPIO to save three microamps.
static constexpr float DIV_RATIO = 390.0f / (1000.0f + 390.0f);   // 0.28058

// At 4.26 V — the most TP4056 can put on the cell — the node sits at 1.196 V, which is inside the
// 0..1300 mV span of 2.5 dB attenuation. That span is worth having: calibrated error is +-12 mV
// against +-23 mV at 12 dB, so +-43 mV on the cell instead of +-82 mV. The acceptance bench uses
// 12 dB deliberately (it must show a fault outside the working range); the product does not.
static constexpr float DIV_MAX_CELL_V = 1.300f / DIV_RATIO;       // 4.63 V, the ceiling of the span

// A gain correction for THIS board's divider, stored in NVS as "bat_cal" and applied to every
// reading. Measured on the first assembled v2 board: the device read 4.07 V against a reference
// 4.00 V, and 4.131 against 4.055 on the charger — the same +1.8 % at both points. Only about
// 0.14 % of that can come from the 0.1 % resistors, so the rest is this converter, and both are
// properties of THIS board rather than of the design: every board is calibrated once, at acceptance.
//
// Two points 55 mV apart cannot separate a gain from a zero offset — solving for both amplifies the
// measurement noise into nonsense — so this is a gain only, and it is honest near the voltage it was
// taken at. A second point far down the curve would settle the question; the discharge run provides
// one.
//
// Clamped hard. The correction feeds BATTERY_CUTOFF_V, the only software guard this cell has, and a
// mistyped reference would move the cutoff anywhere: at 0.75 the device would lock itself out at a
// full cell and refuse to boot far enough to be told otherwise — the same trap that took the
// lowbat_v field out of the console on 2026-09-27. Real errors are under 2 %; +-5 % covers 1 %
// resistors and a bad converter together, and at its worst moves the cutoff to 3.47 V, which is
// merely early.
static constexpr float CAL_MIN = 0.95f;
static constexpr float CAL_MAX = 1.05f;
static constexpr float CAL_NONE = 1.0f;

enum Rev : uint8_t {
    RevUnknown = 0,   // the bus did not come up; decide nothing
    RevGaugeV1 = 1,   // MAX17048 answered at 0x36
    RevDividerV2 = 2, // nothing at 0x36: cell on IO4, TP4056 STDBY# on IO5
};

// Probes 0x36, and — when nothing answers there — a sensor, to tell a v2 board apart from a v1
// board whose gauge has died.
//
// A DEFINITE answer is remembered for the boot; RevUnknown never is. Which board this is cannot
// change while the firmware runs, so a definite answer cannot go stale — but a bus that was down
// for one probe can come back, and caching that would strand the device. The asymmetry is the whole
// design.
//
// Remembering is not an optimisation here, it closes a race. The driver takes the bus lock with a
// timeout, so a probe issued while the loop task is mid-transaction with the CO2 sensor can time
// out. If the 0x36 probe loses that race and a sensor probe wins it, a v1 board reports itself as a
// v2 board and goes looking for a cell on a pin that is not connected to one. Asking once, from the
// loop task, on a quiet bus, is what makes the two-witness test mean anything.
Rev revision(i2c_master_bus_handle_t bus);

// The cell's terminal voltage. False means "could not tell", which is NOT the same as "flat" and
// callers must not treat it as such — see the charger_escape in battery_too_low().
//
// On v2 this is one burst of the divider — 64 conversions, trimmed, converted through a line fitted
// to the calibration curve, multiplied by the board's gain — and, when that lands within 50 mV of
// the cutoff, a second burst 150 ms later with the higher of the two kept. That second look is what
// stops one load pulse from triggering a lockout that only a charger can lift. Safe to call from any
// task: the ADC is taken under a lock. Details in battery_source.cpp.
bool readVoltage(i2c_master_bus_handle_t bus, float &volts);

// Is the charge finished? -1 unknown, 0 charging or no charger, 1 finished.
//
// On v2 the charger says so itself. TP4056's STDBY# (pin 6) is open drain and pulls LOW when the
// charge terminates; the line runs straight to IO5 with NO external pull-up (it was dropped on
// 2026-09-13), so the internal one has to do the job — and only while the cable is in, or we would
// be feeding an unpowered charger through that pin.
//
// Read the pin number twice before trusting this function: on the schematic the net is labelled
// CHRG_DETECT, but it is wired to STDBY# (pin 6), not to CHRG# (pin 7). CHRG# drives the charge
// LED through R18 and goes nowhere else. The two have OPPOSITE meanings — CHRG# is low WHILE
// charging — so a reader who trusts the net name inverts the answer. Traced by geometry in the
// schematic file on 2026-09-29, not by name.
int chargeComplete(bool on_usb);

// The stored gain, or CAL_NONE when there is none or it is out of range.
float calibration();

// Drop everything cached about how this board reads its cell — the gain and the revision. The
// smoothed voltage is not touched: it lives in RTC memory across sleeps on purpose.
//
// Called by the factory reset: the gain is cached from a key the reset erases, and a cache that
// outlives its key would have the device applying a correction it no longer stores, silently, until
// the next boot. The revision goes with it because the bus is re-initialised around that point and
// there is nothing to gain from holding an answer learned before it.
void forgetBoard();

// The divider as the bare hardware reports it, with no gain applied. This is what calibration is
// measured against, and measuring against the corrected value instead would make the operation
// compound: pressing the button twice would square the correction. Fails on a v1 board — the fuel
// gauge carries its own voltage reference and has no business being trimmed by hand.
bool readRawVoltage(i2c_master_bus_handle_t bus, float &volts);

enum CalError : uint8_t {
    CalOk = 0,
    CalRange,       // the reference is not a voltage a lithium cell has, or the gain is past CAL_MIN..MAX
    CalNoDivider,   // a v1 board: the gauge has its own reference
    CalOnCharger,   // the cable is in; see calibrate()
    CalUnreadable,  // the divider would not read, or NVS would not store
    CalUnstable,    // the bursts disagreed: the cell was moving while the meter was being read
};

// What the calibration run saw: how many bursts, their scatter (the radio's doing, on battery) and
// how far the mean moved between the first and second half (a charger or a relaxing cell).
struct CalStats {
    int   n        = 0;
    float sd_mv    = 0.0f;
    float drift_mv = 0.0f;
};

// Store reference_v as the truth for whatever the divider is reading right now. Returns false,
// says why in `why`, and leaves NVS alone unless every check passes. The raw side is the MEAN of 32
// bursts across ~1.5 s — a meter averages the radio's sag the same way — refused only when the mean
// drifts between the two halves of the run (more than 10 mV and four standard errors), never for
// scatter alone. Refused on the charger, where a full ampere through the connector puts the cell and
// the divider tens of millivolts apart. Idempotent: the factor is always reference over the RAW
// reading, so repeating the same measurement yields the same number.
bool calibrate(i2c_master_bus_handle_t bus, float reference_v, float &factor, float &raw,
               CalError *why = nullptr, bool on_usb = false, CalStats *stats = nullptr);

// What the last divider burst looked like, for the log and for anyone checking the reading against a
// meter: the trimmed mean code, the spread of the codes that were kept (the converter's noise as
// this board sees it), how many there were, and the node voltage they came to. valid is false until
// a burst has succeeded in this boot.
struct BurstStats {
    float raw_mean;
    int   spread_lsb;
    int   samples;
    float node_mv;
    bool  valid;
};
BurstStats lastBurst();

// The voltage the percentage is computed from on a v2 board: an exponential average across readings
// with a half-hour time constant, restarted on a cable change, a gap over three hours or a jump over
// 0.1 V, and bypassed on the cable. See battery_source.cpp for why, and for why the lockout does not
// use it. now_s is any clock that keeps running through deep sleep.
float smoothed(float volts, double now_s, bool on_usb);

// Start the average over. Done by calibrate() (the old values are in the old units) and by a
// restart, which re-initialises RTC_DATA; exposed so the host tests can imitate the latter.
// Nothing else is held here: the no-rise hold this comment used to mention was taken out of
// percent() on 2026-10-14 and lives only on the display layer (battery_display.h).
void forgetSmoothing();

// Which cell the board carries, and therefore which measured curve turns its volts into percent.
// Fact from the owner, 2026-10-06: the field v1 board carries the 103450 pack; every v2 board, on
// hand and to come, carries the 804050. The revision decides — there is no other way to tell from
// inside, and the two curves are 35 points apart at the same voltage.
enum Cell : uint8_t {
    Cell804050 = 0,   // v2 boards
    Cell103450 = 1,   // the v1 board
};
Cell cellFor(Rev rev);

// The percentage the device reports, everywhere: screen, console, Telegram, MQTT, history. Both
// boards since 2026-10-08 — before that v1 reported the fuel gauge's own model, which was never
// configured for this cell and sat still on its plateau (1 % in a week, against ~5 % by its volts).
// The curve over smoothed(), then two facts the voltage does not know: a charge the charger has
// finished (charge_done) is 100 %, and a charge it has NOT finished is never 100 %
// (CHARGING_MAX_PCT). Nothing is held back here. This comment claimed a third — "with the cable
// out the percentage never rises, held at the lowest value since" — for five days after that hold
// was removed from the code (2026-10-14); it is a MEASUREMENT, and holding a measurement is what
// put "2 147 483 647 days" on the owner's dashboard. What the owner sees is smoothed on the
// display layer instead, and by a rate limit rather than a latch (battery_display.h).
//
// The ceiling while charging is here and nowhere else, so the screen, Telegram, MQTT, the console
// and the history cannot disagree about it.
static constexpr float CHARGING_MAX_PCT = 99.0f;
float percent(float volts, double now_s, bool on_usb, bool charge_done, Cell cell = Cell804050);

// Percent of the charge the device can still use, from the terminal voltage.
//
// MEASURED on this cell (2026-10-03), not a generic curve: tools/curve_fit.py over a discharge-bench
// run (curve_bench.cpp) on the first v2 bench board — 143 rested readings, 36.9 h, 24.8 h of
// those under a constant radio load. The raw run is kept in soak_2026-10-01_curve/run.bin, so the
// table can be re-fitted with another gain in one command.
//
// The voltages are on the scale of the multimeter the board was calibrated against (gain 0.9879).
// That meter's own error cancels for any board calibrated against the same meter; what it does not
// cancel is the absolute cutoff, and the meter's DC spec is +-(2 % + 5 digits). Re-fit when a better
// reference exists.
//
// What this table cannot know: temperature and age. It describes one cell, at room temperature, new.
//
// Zero is anchored at BATTERY_CUTOFF_V, not at the cell's true empty. The device stops at the
// cutoff, so "0 %" has to mean "the device is about to stop"; anchoring lower would lock the device
// out at a displayed eight percent, which reads as a fault.
//
// The 103450 table (the v1 board's cell) is described where it lives, in the .cpp.
float socFromVoltage(float volts, Cell cell = Cell804050);

// v1 puts the gauge in hibernate; v2 has nothing to park — the divider is passive and the STDBY pin
// is left as an input with its pull-up off.
void park(i2c_master_bus_handle_t bus);

} // namespace BatterySource
