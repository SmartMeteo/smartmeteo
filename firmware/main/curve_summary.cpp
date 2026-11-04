#include "curve_summary.h"
#include <string.h>

namespace CurveSummary {

// tools/curve_fit.py, the same numbers: rests draw about 1.25 % of the load current, the cutoff is
// where 0 % lies. Kept equal on purpose — t_curve.py compares the two on one image.
static constexpr float REST_RATIO = 0.0125f;
static float s_cut = 3.30f;
static constexpr float CV_BAND = 0.010f;     // within 10 mV of the top: the voltage has stopped rising

void set_cutoff(float cut_v) { s_cut = cut_v; }

void Curve::add(float x, float v)
{
    if (++skip < step) return;
    skip = 0;
    if (n == MAXP) {                          // full: keep every other point, and from now on half as many
        for (int i = 0; i < MAXP / 2; i++) p[i] = p[2 * i];
        n = MAXP / 2;
        step *= 2;
    }
    p[n++] = {x, v};
}

float Discharge::mah(float ma) const
{
    const float h = q0_h > 0 ? q0_h : (load_s + REST_RATIO * (float)(t > load_s ? t - load_s : 0)) / 3600.0f;
    return h * ma;
}

float Discharge::wh(float ma) const { return (float)vh * ma / 1000.0f; }

void Charge::integrate()
{
    prog_mah = 0;
    if (!prog_n) return;
    double as = (double)prog[0].a * prog[0].t;                 // flat from the plug-in to the first
    for (int i = 1; i < prog_n; i++)
        as += 0.5 * (prog[i - 1].a + prog[i].a) * (double)(prog[i].t - prog[i - 1].t);
    if (done && t_done > prog[prog_n - 1].t)                   // down to the termination current
        as += 0.5 * (prog[prog_n - 1].a + CHARGE_TERM_A) * (double)(t_done - prog[prog_n - 1].t);
    prog_mah = (float)(as / 3.6);                              // A*s -> mAh
}

uint32_t Charge::prog_gap() const
{
    uint32_t g = 0;
    for (int i = 1; i < prog_n; i++)
        if (prog[i].t - prog[i - 1].t > g) g = prog[i].t - prog[i - 1].t;
    if (done && prog_n && t_done > prog[prog_n - 1].t && t_done - prog[prog_n - 1].t > g)
        g = t_done - prog[prog_n - 1].t;
    return g;
}

bool Charge::prog_covers() const
{
    // The start within a quarter of an hour, and no stretch longer than half an hour without a
    // reading — between two of them or from the last to the charger finishing (PROG_MAX_GAP_S).
    return done && prog_n >= 4 && prog[0].t <= PROG_FIRST_BY_S && prog_gap() <= PROG_MAX_GAP_S;
}

uint32_t prog_next_due(const Charge &c)
{
    static const uint32_t FIXED[] = {120, 600, 1200, 1800};
    const uint32_t after = c.prog_n ? c.prog[c.prog_n - 1].t + 300 : 0;
    for (uint32_t s : FIXED) if (s >= after) return s;
    uint32_t s = 1800;
    while (s < after) s += 900;
    return s;
}

float State::load_ma_measured() const
{
    // C1 refills D1, C2 refills D2: each measured charge that followed its complete discharge is a
    // pair, and the pairs are pooled (sum of charge over sum of hours).
    double mah = 0, h = 0;
    for (int k = 1; k <= 2; k++) {
        const Charge &ch = c[k];
        const Discharge &dd = d[k];
        if (!ch.after_discharge || !ch.prog_covers() || !dd.ended || dd.voided || dd.last_q_h <= 0) continue;
        mah += ch.prog_mah;
        h += dd.last_q_h;
    }
    return h > 0 ? (float)(mah / h) : 0.0f;
}

void State::reset()
{
    any = test = complete = false;
    t_test = t_abs_last = 0;
    seq = 0; records = 0;
    for (auto &x : d) { x = Discharge(); }
    for (auto &x : c) { x = Charge(); }
    re = Charge();
    cur = CurNone; cur_cycle = 0; cur_re = false;
    last_rest_raw = 0; last_pulse_mv = 0; last_was_ended_discharge = false;
}

static void note_temp(float &now, float &lo, float &hi, uint8_t code)
{
    float c;
    if (!CurveLog::temp_of(code, c)) return;
    now = c;
    if (c < lo) lo = c;
    if (c > hi) hi = c;
}

void State::feed(const CurveLog::Rec &r)
{
    any = true;
    records++;
    seq = r.seq;
    using namespace CurveLog;

    if (r.kind == KindTest) {
        // Everything before it belongs to something else (the log is erased right before a test,
        // so in practice there is nothing).
        const uint16_t keep = r.seq;
        reset();
        any = true; records = 1; seq = keep;
        test = true;
        t_test = t_abs_last = r.t;
        return;
    }
    if (!test) return;                        // a v1 run, or anything else from before tests existed

    switch (r.kind) {
    case KindChargeStart: {
        const bool rech = (r.flags & FlagRecharge) != 0;
        const uint8_t cyc = r.spread <= 2 ? r.spread : 0;
        Charge &ch = rech ? re : c[cyc];
        const bool after_discharge = last_was_ended_discharge;
        ch = Charge();
        ch.present = true; ch.recharge = rech; ch.cycle = cyc;
        ch.t0 = r.t; ch.gain = r.gain;
        if (r.v_rest > 2.0f) ch.v_before = r.v_rest * r.gain;
        if (r.v_load > 2.0f) { ch.v_first = r.v_load * r.gain; ch.curve.add(0, ch.v_first); }
        // The charge current at the start is estimated only from a charge that follows a discharge
        // straight away: then the two steps are taken at the same state of charge (see below).
        if (after_discharge) ch.pulse_ref_mv = last_pulse_mv;
        ch.after_discharge = after_discharge;
        cur = CurCharge; cur_cycle = cyc; cur_re = rech;
        t_abs_last = r.t;
        last_was_ended_discharge = false;
        break;
    }
    case KindChargeSample: {
        if (cur != CurCharge) break;
        Charge &ch = charge();
        const float v = r.v_rest * r.gain;
        ch.samples++;
        ch.t = r.t;
        ch.charging_s = r.load_s;
        t_abs_last = ch.t0 + r.t;
        note_temp(ch.temp, ch.t_min, ch.t_max, r.detail);
        if (v <= 2.0f) break;
        ch.v_last = v;
        if (!ch.done) {
            if (v > ch.v_top) ch.v_top = v;
            if (r.t >= 10 && ch.v10 == 0) {
                ch.v10 = v;
                // THE CHARGE CURRENT FROM TWO STEPS. There is no shunt, but the cell's own resistance
                // is one: right after a discharge ends, the 10-second drop under the known load and
                // the 10-second rise on the charger are taken at the same state of charge, so their
                // ratio is the ratio of the currents. Assumes the resistance is the same both ways and
                // linear in current — it is not quite (charge transfer is sub-linear at an ampere), so
                // this tends to read low. It says whether the charger gives an ampere or is held back.
                if (ch.pulse_ref_mv > 1.0f && ch.v_before > 2.0f && v > ch.v_before)
                    ch.i_est_ma = LOAD_MA * (v - ch.v_before) * 1000.0f / ch.pulse_ref_mv;
            }
            ch.curve.add(r.t / 60.0f, v);
        } else {
            ch.v_relaxed = v;
            ch.relaxed_s = r.t > ch.t_done ? r.t - ch.t_done : 0;
            if (r.t <= ch.t_done + 1800) ch.curve.add(r.t / 60.0f, v);   // the graph: half an hour of it
        }
        break;
    }
    case KindChargeEnd: {
        if (cur != CurCharge) break;
        Charge &ch = charge();
        ch.done = true;
        ch.t_done = r.t; ch.t = r.t;
        t_abs_last = ch.t0 + r.t;
        if (r.v_rest > 2.0f) ch.v_done = r.v_rest * r.gain;
        if (r.v_load * r.gain > ch.v_top) ch.v_top = r.v_load * r.gain;
        // The end of constant current: the first reading within 10 mV of the top. Before it the
        // voltage climbs with the charge; after it TP4056 holds its float voltage and the current
        // falls instead.
        for (int i = 0; i < ch.curve.n; i++)
            if (ch.curve.p[i].v >= ch.v_top - CV_BAND) { ch.t_cv = (uint32_t)(ch.curve.p[i].x * 60.0f + 0.5f); break; }
        if (ch.cycle == 2 && !ch.recharge) complete = true;
        ch.integrate();
        break;
    }
    case KindProg: {
        if (cur != CurCharge) break;
        Charge &ch = charge();
        t_abs_last = ch.t0 + r.t;
        // After the end the charger is idle and there is nothing to read; a reading out of order or
        // out of the meter's sensible range (the programmed current is 1 A) is a typo, not a current.
        if (ch.done || ch.prog_n >= MAXPROG || !(r.v_load >= 0.0f && r.v_load <= 1.5f)) break;
        if (ch.prog_n && r.t < ch.prog[ch.prog_n - 1].t) break;
        ch.prog[ch.prog_n++] = {r.t, r.v_load * PROG_A_PER_V};
        ch.integrate();
        break;
    }
    case KindUnplug:
        if (cur != CurCharge) break;
        charge().cut = true;
        charge().idle = r.detail == 1;
        break;
    case KindTestDone:
        complete = true;
        break;

    case KindHeader: {
        const uint8_t cyc = (r.spread == 1 || r.spread == 2) ? r.spread : 1;
        Discharge &x = d[cyc];
        x = Discharge();
        x.present = true; x.cycle = cyc; x.t0 = r.t; x.gain = r.gain;
        cur = CurDischarge; cur_cycle = cyc; cur_re = false;
        t_abs_last = r.t;
        last_was_ended_discharge = false;
        break;
    }
    case KindSample: {
        if (cur != CurDischarge) break;
        Discharge &x = discharge();
        const uint32_t prev_load = x.load_s;
        x.t = r.t; x.load_s = r.load_s; x.gain = r.gain;
        t_abs_last = x.t0 + r.t;
        note_temp(x.temp, x.t_min, x.t_max, r.detail);
        if (r.v_rest <= 2.0f) break;
        const float v = r.v_rest * r.gain;
        last_rest_raw = r.v_rest;
        // Running minimum, as the tool: a rested reading only falls on a discharge, a rise is noise.
        const float vm = (x.samples && x.v_last < v) ? x.v_last : v;
        const float q_h = (r.load_s + REST_RATIO * (float)(r.t > r.load_s ? r.t - r.load_s : 0)) / 3600.0f;
        if (x.samples == 0) x.v_full = v;
        else if (x.q0_h == 0 && vm <= s_cut && x.v_last > s_cut) {
            // 0 %: the crossing, between the previous sample and this one.
            const float a = x.v_last, qa = x.last_q_h;
            x.q0_h = (a == vm) ? q_h : qa + (a - s_cut) / (a - vm) * (q_h - qa);
        }
        if (r.v_load > 2.0f) {
            const float vl = r.v_load * r.gain;
            x.v_load_last = vl;
            x.sag_mv = (v - vl) * 1000.0f;
            x.sag_sum += x.sag_mv; x.sag_n++;
            x.vh += (double)vl * (double)(r.load_s - prev_load) / 3600.0;
        }
        x.v_last = vm;
        x.last_q_h = q_h;
        x.samples++;
        x.curve.add(r.load_s / 3600.0f, vm);
        break;
    }
    case KindPulse: {
        if (cur != CurDischarge) break;
        Discharge &x = discharge();
        t_abs_last = x.t0 + r.t;
        if (r.v_rest > 2.0f && r.v_load > 2.0f) {
            x.pulse_mv = (r.v_rest - r.v_load) * r.gain * 1000.0f;
            x.pulse_sum += x.pulse_mv; x.pulse_n++;
            x.pulses++;
            last_pulse_mv = x.pulse_mv;
        }
        break;
    }
    case KindEnd:
        if (cur != CurDischarge) break;
        discharge().ended = true;
        last_was_ended_discharge = true;
        break;
    case KindCharger:
        if (cur != CurDischarge) break;
        discharge().voided = true;
        break;
    case KindReset:
        if (cur == CurCharge) charge().resets++;
        else if (cur == CurDischarge) discharge().resets++;
        break;
    default:
        break;
    }
}

Next next(const State &s)
{
    if (!s.any) return Next::Empty;
    if (!s.test) return Next::OldLog;
    if (s.complete) return Next::Complete;
    switch (s.cur) {
    case State::CurNone:
        return Next::Begin;
    case State::CurCharge: {
        const Charge &ch = s.charge();
        if (ch.done) return Next::ChargedFull;
        if (ch.cut) return ch.idle ? Next::RunDown : Next::ChargeCut;
        return Next::Charging;
    }
    case State::CurDischarge: {
        const Discharge &x = s.discharge();
        if (x.voided) return Next::DischargeVoid;
        if (x.ended) return Next::DischargeDone;
        return Next::Discharging;
    }
    }
    return Next::Begin;
}

const char *name(Next n)
{
    switch (n) {
    case Next::Empty:         return "empty";
    case Next::OldLog:        return "old log";
    case Next::Begin:         return "begin";
    case Next::Charging:      return "charging";
    case Next::ChargedFull:   return "charged full";
    case Next::ChargeCut:     return "charge cut";
    case Next::RunDown:       return "run down";
    case Next::Discharging:   return "discharging";
    case Next::DischargeDone: return "discharge done";
    case Next::DischargeVoid: return "discharge void";
    case Next::Complete:      return "complete";
    }
    return "?";
}

}  // namespace CurveSummary
