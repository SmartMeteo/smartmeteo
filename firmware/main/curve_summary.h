#pragma once
// What the battery bench's log says, worked out in one place.
//
// The bench (curve_bench.cpp) asks it two things after every boot and every cable event: what the
// screen should show, and what to do next. Both come from the log alone — nothing the bench needs
// lives only in RAM, so a reset, a brownout or a reflash in the middle of a four-day test costs one
// line in the log and nothing else. tools/curve_fit.py computes the same numbers on the Mac; the
// host tests feed one image to both and compare (t_cycle → t_curve.py).
//
// No ESP-IDF here, on purpose: this is the part of the bench that can be tested without a board.
//
// A TEST is: C0 charge → D1 discharge → C1 charge → D2 discharge → C2 charge. C0 starts wherever
// the cell happens to be, so its time means nothing; C1 and C2 start at the cutoff and are the
// measured charges. D1 and D2 start from a charge the bench watched end and are the measured
// discharges. The cable has to be moved by hand at every arrow: on this board the charger cannot be
// switched off (CE is tied to VBUS) and the cell is disconnected from the load whenever USB is in.

#include "curve_log.h"
#include <stdint.h>

namespace CurveSummary {

// The load current is NOT measured — there is no shunt on the board. It is the radio receiver kept
// on (ESP32-C6-WROOM-1 datasheet v1.4, table 6-4: Wi-Fi RX 78 mA) plus the regulator and the rest of
// the board, and the first run's capacity (2.0 Ah to the cutoff at this figure) agrees with the cell's
// label. Every mAh and Wh the bench shows is this number times a measured time — an estimate, and
// labelled as one. What does NOT depend on it: times, voltages, and the ratio between two cells run
// on the same board, which is the comparison the bench is for.
static constexpr float LOAD_MA = 82.0f;

// The charge current can be MEASURED, by hand: TP4056's datasheet names PROG the "Charge Current
// Monitor Pin" — in every charge mode I_BAT = V_PROG / R_PROG x 1200 — and on board v2 PROG goes to
// ground through R7, 1.2 kOhm 1 % (BOM 0805W8F1201T5E). So the volts across R7 are the amperes, one
// for one. The datasheet's pin description also says "regulated to 2 V"; its formula and its table
// (1.2 kOhm -> 1000 mA) both say 1 V, and agree with each other, so the 2 V is taken for a slip.
static constexpr float PROG_A_PER_V = 1.0f;
static constexpr float CHARGE_TERM_A = 0.1f;   // TP4056 stops at a tenth of the programmed current
static constexpr int   MAXPROG = 48;
// How much of a charge the R7 readings may leave uncovered before their integral stops being a
// measurement. The schedule asks every 15 minutes; half an hour between two readings (or from the
// last one to the charger finishing) forgives one missed reading and no more. Without this rule a
// charge read at 0:02, 0:10, 1:24 and 1:30 — board f708's C0, 2026-10-03, the owner out of the house
// in between — passed: the integral drew a straight line across the hour where the constant current
// turns into the falling one, and would have reported 0.56 Ah where the truth was anywhere up to 0.8.
// On C1 or C2 that silent error goes straight into the measured load current, and from there into
// the comparison of the two cells. tools/curve_fit.py holds the same two numbers; t_curve.py checks.
static constexpr uint32_t PROG_FIRST_BY_S = 900;
static constexpr uint32_t PROG_MAX_GAP_S  = 1800;

static constexpr int MAXP = 200;      // points kept per segment for the graph; decimated beyond

struct Point { float x, v; };         // x: hours under load (discharge), minutes (charge)

struct Curve {
    Point p[MAXP];
    int   n = 0;
    int   step = 1, skip = 0;         // decimation: keep one point in `step`
    void clear() { n = 0; step = 1; skip = 0; }
    void add(float x, float v);
};

struct Discharge {
    bool     present = false, ended = false, voided = false;
    uint8_t  cycle = 0;
    uint32_t t0 = 0;                  // absolute clock at the start
    uint32_t t = 0;                   // wall seconds since the start, last record
    uint32_t load_s = 0;              // seconds under load
    int      samples = 0, pulses = 0, resets = 0;
    float    gain = 1.0f;
    float    v_full = 0;              // first rested reading, with the gain
    float    v_last = 0;              // last rested reading, with the gain
    float    v_load_last = 0;         // last loaded reading (end of a load phase), with the gain
    float    sag_mv = 0;              // last rested minus the loaded one before it
    float    sag_sum = 0; int sag_n = 0;
    float    pulse_mv = 0;            // last 10-second drop under load
    float    pulse_sum = 0; int pulse_n = 0;
    float    temp = -100, t_min = 100, t_max = -100;
    double   vh = 0;                  // integral of the loaded voltage over hours under load (V·h)
    float    q0_h = 0;                // hours under load to the cutoff, interpolated; 0 = not reached
    float    last_q_h = 0;            // the charge axis at the last sample (load hours + the rests' share)
    Curve    curve;
    float    mah(float ma = LOAD_MA) const;   // q0_h (or load hours so far) times the load current
    float    wh(float ma = LOAD_MA) const;
};

struct Charge {
    bool     present = false, done = false, cut = false, idle = false, recharge = false;
    uint8_t  cycle = 0;
    uint32_t t0 = 0;                  // absolute clock at the start
    uint32_t t = 0;                   // seconds since the start, last record
    uint32_t charging_s = 0;          // seconds the charger was seen charging
    uint32_t t_done = 0;              // the charge time (STDBY# low)
    uint32_t t_cv = 0;                // when the voltage reached its plateau: the end of constant current
    int      samples = 0, resets = 0;
    float    gain = 1.0f;
    float    v_before = 0;            // rested, just before the cable went in
    float    v_first = 0;             // the first reading on the charger
    float    v10 = 0;                 // the first reading 10 s or more into the charge
    float    v_top = 0;               // highest reading on the charger
    float    v_last = 0;
    float    v_done = 0;
    float    v_relaxed = 0;           // the latest reading after the end, cable still in, cell unloaded
    uint32_t relaxed_s = 0;           // ...and how long after the end it was taken
    float    temp = -100, t_min = 100, t_max = -100;
    bool     after_discharge = false; // started right after a complete discharge: refills exactly what it took
    float    pulse_ref_mv = 0;        // the last 10-s pulse of the discharge right before this charge
    float    i_est_ma = 0;            // charge current at the start, from the two steps (see .cpp)
    // Readings of the charge current off R7, entered by hand. Integrated into the charge put in:
    // flat from 0 to the first reading, trapezoids between readings, and when the charger has
    // finished, a straight line from the last reading down to its termination current at t_done.
    struct Prog { uint32_t t; float a; };
    Prog     prog[MAXPROG];
    int      prog_n = 0;
    float    prog_mah = 0;            // the charge put in, from the readings; 0 = none
    bool     prog_covers() const;     // enough readings, across the whole charge, to trust prog_mah
    uint32_t prog_gap() const;        // the longest stretch the readings leave uncovered, seconds
    void     integrate();
    Curve    curve;
};

// When the next reading of the charge current is due, in seconds since the charge began: at 2, 10,
// 20 and 30 minutes, then every 15 until the charger finishes — and never sooner than 5 minutes
// after the last reading, wherever it fell. Dense at the start, where TP4056 may be holding its
// current back for heat, and through constant voltage, where the current falls fastest.
uint32_t prog_next_due(const Charge &c);

enum class Next : uint8_t {
    Empty,          // nothing logged: the cable going in starts a test
    OldLog,         // something is logged but no test (a v1 run): read it, then erase with the button
    Begin,          // a test record but no segment yet (reset right after it): start C0
    Charging,
    ChargedFull,    // the charger has finished: pulling the cable starts the next discharge
    ChargeCut,      // the cable came out before the end: plug it back in
    RunDown,        // the charger was idle at the start (cell above its restart point): run it down
    Discharging,
    DischargeDone,  // cutoff reached: plug in for the next charge
    DischargeVoid,  // the cable went in mid-discharge: recharge, then the same discharge again
    Complete,
};

struct State {
    bool     any = false;             // at least one valid record
    bool     test = false;            // a KindTest record
    bool     complete = false;
    uint32_t t_test = 0;              // absolute clock at the start of the test
    uint32_t t_abs_last = 0;          // absolute clock of the last record
    uint16_t seq = 0;
    int      records = 0;
    Discharge d[3];                   // [1], [2]: the latest attempt of each
    Charge    c[3];                   // [0], [1], [2]: the latest of each, re-charges excluded
    Charge    re;                     // the latest re-charge after a voided discharge
    enum Cur : uint8_t { CurNone, CurCharge, CurDischarge } cur = CurNone;
    uint8_t  cur_cycle = 0;
    bool     cur_re = false;
    float    last_rest_raw = 0;       // the last rested reading, raw — what the next charge started from
    float    last_pulse_mv = 0;       // with the gain
    bool     last_was_ended_discharge = false;

    void      reset();
    void      feed(const CurveLog::Rec &r);
    // The load current of the discharges, MEASURED: a charge from the cutoff puts back what the
    // discharge before it took (coulombic efficiency of lithium is above 99 %), so the charge's
    // milliamp-hours over the discharge's hours on the charge axis is the load current. 0 = not yet
    // known (no measured charge right after a complete discharge). load_ma() falls back to LOAD_MA.
    float     load_ma_measured() const;
    float     load_ma() const { const float m = load_ma_measured(); return m > 0 ? m : LOAD_MA; }
    Charge   &charge()       { return cur_re ? re : c[cur_cycle]; }
    const Charge &charge() const { return cur_re ? re : c[cur_cycle]; }
    Discharge &discharge()   { return d[cur_cycle]; }
    const Discharge &discharge() const { return d[cur_cycle]; }
};

Next next(const State &s);
const char *name(Next n);

// The voltage at which a discharge counts as finished, and the one where 0 % lies. Defaults are
// the bench's: 3.20 V rested with a calibrated divider (BATTERY_CUTOFF_V is 3.30, the margin puts the
// crossing inside the run), and the cutoff itself for the capacity.
void set_cutoff(float cut_v);

}  // namespace CurveSummary
