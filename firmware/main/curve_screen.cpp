// The battery bench's screen, 400 x 300. One layout for every phase, so the eye learns where things
// are once:
//
//   bar      what the bench is doing, the cycle, how long the test has run
//   left     the cell voltage, large, and what kind of reading it is
//   right    seven lines of numbers for the phase in progress
//   graph    the voltage of this segment against its own axis, the same segment of the other cycle
//            dotted behind it — so the second discharge is drawn over the first as it happens
//   tables   every discharge and every charge of the test so far
//   footer   what the estimate rests on, and what the person must do next, inverted when it matters
//
// Numbers that are estimates say so with a tilde: every mAh and Wh is a measured time times an
// assumed current (CurveSummary::LOAD_MA), and the charge current comes from two voltage steps.
#include "curve_screen.h"
#include "org01.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace CurveScreen {

using namespace CurveSummary;

static constexpr int W = 400;

// ---- small drawing helpers ---------------------------------------------------------------------

static int tx(EPaper &e, int x, int y, const char *s, int size = 1, uint8_t c = 0)
{
    return e.drawString(x, y, s, c, size);
}

static void rtx(EPaper &e, int xr, int y, const char *s, int size = 1, uint8_t c = 0)
{
    e.drawString(xr - (int)strlen(s) * 6 * size, y, s, c, size);
}

static void ctx(EPaper &e, int xc, int y, const char *s, int size = 1, uint8_t c = 0)
{
    e.drawString(xc - (int)strlen(s) * 3 * size, y, s, c, size);
}

// h:mm, or h:mm:ss
static void hm(char *out, size_t n, uint32_t s) { snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60)); }
static void hms(char *out, size_t n, uint32_t s)
{
    snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
}

static void line(EPaper &e, int x0, int y0, int x1, int y1, bool dotted, int &phase)
{
    // Bresenham, so a dotted line keeps its rhythm across segments (phase carries over).
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (!dotted || (phase++ % 4) < 2) e.drawPixel(x0, y0, 0);
        if (!dotted) e.drawPixel(x0, y0 + 1, 0);             // the live curve, two pixels thick
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

struct Plot {
    int x0, y0, w, h;          // the box, inside the frame
    float xmax, vlo, vhi;
    int px(float x) const { return x0 + (int)lroundf(x / xmax * (w - 1)); }
    int py(float v) const
    {
        if (v < vlo) v = vlo;
        if (v > vhi) v = vhi;
        return y0 + h - 1 - (int)lroundf((v - vlo) / (vhi - vlo) * (h - 1));
    }
};

static void plot_curve(EPaper &e, const Plot &p, const Curve &c, bool dotted, float x_last = -1, float v_last = 0)
{
    int phase = 0;
    for (int i = 1; i < c.n; i++)
        line(e, p.px(c.p[i - 1].x), p.py(c.p[i - 1].v), p.px(c.p[i].x), p.py(c.p[i].v), dotted, phase);
    // The decimated curve may stop short of the latest reading: finish it there.
    if (c.n && x_last > c.p[c.n - 1].x && v_last > 2.0f)
        line(e, p.px(c.p[c.n - 1].x), p.py(c.p[c.n - 1].v), p.px(x_last), p.py(v_last), dotted, phase);
}

static void frame(EPaper &e, const Plot &p, float tick, const char *unit, float cut_v)
{
    e.drawRect(p.x0 - 1, p.y0 - 1, p.w + 2, p.h + 2, 0);
    // Voltage gridlines every 0.1 V, dotted; labels on the round ones.
    for (int mv = (int)ceilf(p.vlo * 10); mv <= (int)floorf(p.vhi * 10); mv++) {
        const float v = mv / 10.0f;
        const int y = p.py(v);
        for (int x = p.x0; x < p.x0 + p.w; x += 4) e.drawPixel(x, y, 0);
        if (mv % 5 == 0) {
            char b[16]; snprintf(b, sizeof(b), "%.1f", v);
            rtx(e, p.x0 - 3, y - 3, b);
        }
    }
    // The cutoff, dashed heavier: 0 % lies there.
    const int yc = p.py(cut_v);
    for (int x = p.x0; x < p.x0 + p.w; x += 6) e.drawLine(x, yc, x + 2, yc, 0);
    // Time ticks below the box.
    for (float t = 0; t <= p.xmax + 0.001f; t += tick) {
        const int x = p.px(t);
        e.drawLine(x, p.y0 + p.h, x, p.y0 + p.h + 2, 0);
        char b[16]; snprintf(b, sizeof(b), "%g", t);
        const int half = (int)strlen(b) * 3;
        if (x + half > W - 1) rtx(e, W - 1, p.y0 + p.h + 4, b);
        else ctx(e, x, p.y0 + p.h + 4, b);
    }
    tx(e, 1, p.y0 + p.h + 4, unit);
}

// Top right inside the box: the solid curve, and the dotted one behind it.
static void legend(EPaper &e, const Plot &p, const char *solid, const char *dotted)
{
    const int y = p.y0 + 6, xr = p.x0 + p.w;
    e.fillRect(xr - 74, p.y0 + 1, 73, 11, 1);
    e.drawLine(xr - 70, y, xr - 58, y, 0);
    e.drawLine(xr - 70, y + 1, xr - 58, y + 1, 0);
    tx(e, xr - 54, y - 3, solid);
    if (dotted) {
        for (int x = xr - 34; x < xr - 22; x += 4) { e.drawPixel(x, y, 0); e.drawPixel(x + 1, y, 0); }
        tx(e, xr - 18, y - 3, dotted);
    }
}

// ---- the parts ---------------------------------------------------------------------------------

static const char *alert_for(const State &s, const View &v, char *buf, size_t n)
{
    switch (v.next) {
    case Next::Empty:         return v.usb ? "STARTING..." : "PLUG IN TO START THE TEST";
    case Next::OldLog:        return "OLD LOG: HOLD BUTTON 3 S";
    case Next::Begin:         return "PLUG IN TO START THE TEST";
    case Next::Charging:      return "CHARGING - KEEP CABLE IN";
    case Next::ChargedFull:
        snprintf(buf, n, "FULL - PULL CABLE: STARTS D%d", s.charge().cycle + 1);
        return buf;
    case Next::ChargeCut:     return "NOT FULL - PLUG BACK IN";
    case Next::RunDown:       return v.usb ? "UNPLUG: RUN IT DOWN FIRST" : "RUNNING DOWN - CABLE OUT";
    case Next::Discharging:
        snprintf(buf, n, "D%d RUNNING - DO NOT PLUG IN", s.cur_cycle);
        return buf;
    case Next::DischargeDone: return "PLUG THE CABLE IN NOW";
    case Next::DischargeVoid:
        snprintf(buf, n, "VOID - PLUG IN, D%d AGAIN", s.cur_cycle);
        return buf;
    case Next::Complete:      return v.usb ? "COMPLETE - READ THE LOG" : "TEST COMPLETE";
    }
    return "";
}

static void title_bar(EPaper &e, const State &s, const View &v)
{
    e.fillRect(0, 0, W, 20, 0);
    char t[48] = "BATTERY TEST";
    if (s.test) {
        switch (s.cur) {
        case State::CurCharge:
            snprintf(t, sizeof(t), "C%d %s", s.cur_cycle,
                     s.charge().done ? "CHARGED" : s.cur_re ? "RECHARGE" : "CHARGE");
            break;
        case State::CurDischarge:
            snprintf(t, sizeof(t), "D%d DISCHARGE", s.cur_cycle);
            break;
        default: break;
        }
        if (s.complete) snprintf(t, sizeof(t), "TEST COMPLETE");
    }
    tx(e, 4, 3, t, 2, 1);
    char a[48], b[48];
    if (s.test && v.now_abs > s.t_test) {
        const float h = (v.now_abs - s.t_test) / 3600.0f;
        snprintf(a, sizeof(a), "test %.1f h", h);
    } else snprintf(a, sizeof(a), "battery bench");
    const int cyc = s.cur == State::CurDischarge ? s.cur_cycle : s.cur == State::CurCharge ? s.cur_cycle : 0;
    if (!s.test) b[0] = 0;
    else if (s.complete) snprintf(b, sizeof(b), "2 of 2 cycles");
    else if (s.cur == State::CurCharge && cyc == 0 && !s.cur_re) snprintf(b, sizeof(b), "first charge");
    else snprintf(b, sizeof(b), "cycle %d of 2", cyc ? cyc : 1);
    rtx(e, W - 4, 2, a, 1, 1);
    rtx(e, W - 4, 11, b, 1, 1);
}

static void big_value(EPaper &e, const char *num, const char *unit, const char *cap1, const char *cap2, const char *cap3)
{
    const int x = 4, base = 50;
    const int uw = (int)strlen(unit) * 12;
    int size = 6;
    while (size > 3 && x + e.gfxStrWidth(num, &Org_01, size) + 4 + uw > 176) size--;
    const int end = e.drawGFXString(x, base, num, &Org_01, 0, size);
    tx(e, end + 4, base - 14, unit, 2);
    tx(e, x, 62, cap1);
    tx(e, x, 72, cap2);
    tx(e, x, 82, cap3);
}

static void right_lines(EPaper &e, const char *const *l, int n)
{
    for (int i = 0; i < n; i++) if (l[i]) tx(e, 180, 25 + i * 10, l[i]);
}

static void discharge_view(EPaper &e, const State &s, const View &v)
{
    const Discharge &d = s.discharge();
    const float vnow = v.v_now > 2.0f ? v.v_now : d.v_last;
    char num[24], c1[64], c2[64], c3[64];
    snprintf(num, sizeof(num), "%.3f", vnow);
    snprintf(c1, sizeof(c1), "%s", v.v_now > 2.0f ? "cell now" : "rested 5 min");
    if (d.v_load_last > 2.0f) snprintf(c2, sizeof(c2), "under load %.3f V", d.v_load_last);
    else snprintf(c2, sizeof(c2), "under load: not yet");
    if (d.v_full > 2.0f) snprintf(c3, sizeof(c3), "full %.3f  end %.2f V", d.v_full, v.end_v);
    else snprintf(c3, sizeof(c3), "first rest in 5 min, end %.2f V", v.end_v);
    big_value(e, num, "V", c1, c2, c3);

    char l[7][96];
    snprintf(l[0], sizeof(l[0]), "load   %6.2f h  wall %.2f h", d.load_s / 3600.0f, d.t / 3600.0f);
    const float ma = s.load_ma();
    const char *tl = s.load_ma_measured() > 0 ? "" : "~";
    snprintf(l[1], sizeof(l[0]), "used   %s%4.0f mAh  %s%.2f Wh", tl, d.mah(ma), tl, d.wh(ma));
    if (d.sag_n) snprintf(l[2], sizeof(l[0]), "sag    %3.0f mV after 15 min", d.sag_mv);
    else         snprintf(l[2], sizeof(l[0]), "sag    -- (after the 1st load)");
    if (d.pulse_n) snprintf(l[3], sizeof(l[0]), "pulse  %3.0f mV at 10 s %s%.2f ohm", d.pulse_mv, tl, d.pulse_mv / ma);
    else           snprintf(l[3], sizeof(l[0]), "pulse  -- (10 s into a load)");
    if (d.temp > -50) snprintf(l[4], sizeof(l[0]), "temp   %.1f C  (%.1f..%.1f)", d.temp, d.t_min, d.t_max);
    else              snprintf(l[4], sizeof(l[0]), "temp   --");
    snprintf(l[5], sizeof(l[0]), "points %d   resets %d", d.samples, d.resets);
    if (d.ended)       snprintf(l[6], sizeof(l[0]), "DONE at %.3f V", d.v_last);
    else if (d.voided) snprintf(l[6], sizeof(l[0]), "VOID: the cable went in");
    else               snprintf(l[6], sizeof(l[0]), "next   %d min load, 5 min rest", v.next_load_min);
    const char *pl[7] = {l[0], l[1], l[2], l[3], l[4], l[5], l[6]};
    right_lines(e, pl, 7);

    // The graph: this discharge against the other one.
    const Discharge &o = s.d[d.cycle == 1 ? 2 : 1];
    float xmax = d.load_s / 3600.0f;
    if (o.present && o.load_s / 3600.0f > xmax) xmax = o.load_s / 3600.0f;
    xmax = xmax < 25 ? 25 : ceilf(xmax * 1.05f / 5) * 5;
    Plot p = {26, 98, 370, 96, xmax, 3.2f, 4.25f};
    frame(e, p, 5, "h", 3.30f);
    if (o.present && o.curve.n) plot_curve(e, p, o.curve, true);
    plot_curve(e, p, d.curve, false, d.load_s / 3600.0f, d.v_last);
    char a[8], b[8];
    snprintf(a, sizeof(a), "D%d", d.cycle);
    snprintf(b, sizeof(b), "D%d", o.cycle);
    legend(e, p, a, o.present ? b : nullptr);
}

static void charge_view(EPaper &e, const State &s, const View &v)
{
    const Charge &c = s.charge();
    const float vnow = v.v_now > 2.0f ? v.v_now : c.v_last;
    char num[24], c1[64], c2[64], c3[64];
    snprintf(num, sizeof(num), "%.3f", vnow);
    snprintf(c1, sizeof(c1), "%s", c.done ? "after the charge" : "on the charger");
    if (c.v_before > 2.0f) snprintf(c2, sizeof(c2), "before: %.3f V rested", c.v_before);
    else snprintf(c2, sizeof(c2), "before: unknown");
    snprintf(c3, sizeof(c3), "top so far %.3f V", c.v_top);
    big_value(e, num, "V", c1, c2, c3);

    // CC or CV, live: has the voltage still risen in the last ten minutes?
    bool cv = false;
    float rise = 0;
    if (c.curve.n >= 2 && !c.done) {
        const Point &last = c.curve.p[c.curve.n - 1];
        int i = c.curve.n - 1;
        while (i > 0 && c.curve.p[i].x > last.x - 10.0f) i--;
        rise = (last.v - c.curve.p[i].v) * 1000.0f;
        cv = last.x - c.curve.p[i].x >= 9.0f && rise < 5.0f && last.v > 4.05f;
    }
    char l[7][96], a[24], b[24];
    hms(a, sizeof(a), c.done ? c.t_done : c.t);
    hm(b, sizeof(b), c.charging_s);
    snprintf(l[0], sizeof(l[0]), "time   %s  charging %s", a, b);
    if (c.done) {
        hm(a, sizeof(a), c.t_cv); hm(b, sizeof(b), c.t_done - c.t_cv);
        snprintf(l[1], sizeof(l[0]), "DONE   CC %s + CV %s", a, b);
    } else if (cv) snprintf(l[1], sizeof(l[0]), "phase  CV, holding %.3f V", c.v_last);
    else snprintf(l[1], sizeof(l[0]), "phase  CC, +%.0f mV / 10 min", rise);
    if (c.v10 > 2.0f && c.v_before > 2.0f && c.i_est_ma > 0)
        snprintf(l[2], sizeof(l[0]), "step   %+.0f mV at 10 s ~%.0f mA", (c.v10 - c.v_before) * 1000.0f, c.i_est_ma);
    else if (c.v10 > 2.0f && c.v_before > 2.0f)
        snprintf(l[2], sizeof(l[0]), "step   %+.0f mV at 10 s", (c.v10 - c.v_before) * 1000.0f);
    else snprintf(l[2], sizeof(l[0]), "step   --");
    // The current itself, as read off R7 (1 V = 1 A): the last reading while charging, the whole
    // charge put in once it is over.
    if (c.prog_n && c.done && c.prog_covers())
        snprintf(l[3], sizeof(l[0]), "R7     %.0f mAh in, %d readings", c.prog_mah, c.prog_n);
    else if (c.prog_n) {
        const Charge::Prog &p = c.prog[c.prog_n - 1];
        char at[24]; hm(at, sizeof(at), p.t);
        snprintf(l[3], sizeof(l[0]), "R7     %.2f A  #%d at %s", p.a, c.prog_n, at);
    } else snprintf(l[3], sizeof(l[0]), "R7     -- (measure, enter on page)");
    if (c.done && c.relaxed_s) { hm(a, sizeof(a), c.relaxed_s); snprintf(l[4], sizeof(l[0]), "rested %.3f V, %s after", c.v_relaxed, a); }
    else snprintf(l[4], sizeof(l[0]), "rested -- (after the end)");
    if (c.temp > -50) snprintf(l[5], sizeof(l[0]), "temp   %.1f C  (%.1f..%.1f)", c.temp, c.t_min, c.t_max);
    else snprintf(l[5], sizeof(l[0]), "temp   --");
    if (c.cut) snprintf(l[6], sizeof(l[0]), "%s", c.idle ? "idle: cell above 4.05 V, run down" : "cable out before the end");
    else snprintf(l[6], sizeof(l[0]), "STDBY  %s  resets %d", c.done ? "low: finished" : "high: charging", c.resets);
    const char *pl[7] = {l[0], l[1], l[2], l[3], l[4], l[5], l[6]};
    right_lines(e, pl, 7);

    // The graph: this charge against the other measured one (C1 against C2, C0 against nothing).
    const Charge *o = nullptr;
    if (!c.recharge && c.cycle == 2 && s.c[1].present) o = &s.c[1];
    if (!c.recharge && c.cycle == 1 && s.c[2].present) o = &s.c[2];
    float xmax = (c.done ? c.t_done + 1800 : c.t) / 60.0f;
    if (o && o->curve.n && o->curve.p[o->curve.n - 1].x > xmax) xmax = o->curve.p[o->curve.n - 1].x;
    xmax = xmax < 180 ? 180 : ceilf(xmax * 1.05f / 30) * 30;
    Plot p = {26, 98, 370, 96, xmax, 3.2f, 4.25f};
    frame(e, p, xmax > 300 ? 60 : 30, "min", 3.30f);
    if (o && o->curve.n) plot_curve(e, p, o->curve, true);
    plot_curve(e, p, c.curve, false, c.done ? -1 : c.t / 60.0f, c.v_last);
    char la[8], lb[8];
    snprintf(la, sizeof(la), "C%d", c.cycle);
    if (o) snprintf(lb, sizeof(lb), "C%d", o->cycle);
    legend(e, p, la, o ? lb : nullptr);
    if (c.done) {                              // where the charger said it had finished
        const int x = p.px(c.t_done / 60.0f);
        for (int y = p.y0; y < p.y0 + p.h; y += 3) e.drawPixel(x, y, 0);
        tx(e, x + 3, p.y0 + p.h - 10, "end");
    }
}

static void complete_view(EPaper &e, const State &s, const View &v)
{
    const Discharge &a = s.d[1], &b = s.d[2];
    float mean = 0; int n = 0;
    const float ma = s.load_ma();
    const bool meas = s.load_ma_measured() > 0;
    if (a.q0_h > 0) { mean += a.mah(ma); n++; }
    if (b.q0_h > 0) { mean += b.mah(ma); n++; }
    char num[24], c1[64], c2[64], c3[64];
    if (n) snprintf(num, sizeof(num), "%.0f", mean / n); else snprintf(num, sizeof(num), "--");
    snprintf(c1, sizeof(c1), "%s", meas ? "capacity, D1/D2 mean" : "~ capacity, D1/D2 mean");
    snprintf(c2, sizeof(c2), "= hours to %.2f V x %.1f mA%s", 3.30f, ma, meas ? "" : " est");
    if (a.q0_h > 0 && b.q0_h > 0) snprintf(c3, sizeof(c3), "D2 / D1 = %.1f %%", 100.0f * b.q0_h / a.q0_h);
    else c3[0] = 0;
    big_value(e, num, "mAh", c1, c2, c3);

    char l[7][96], t1[24], t2[24];
    snprintf(l[0], sizeof(l[0]), "D1     %.2f h load  %s%.2f Wh", a.q0_h, meas ? "" : "~", a.wh(ma));
    snprintf(l[1], sizeof(l[0]), "D2     %.2f h load  %s%.2f Wh", b.q0_h, meas ? "" : "~", b.wh(ma));
    hm(t1, sizeof(t1), s.c[1].t_done); hm(t2, sizeof(t2), s.c[2].t_done);
    snprintf(l[2], sizeof(l[0]), "charge C1 %s  C2 %s", t1, t2);
    snprintf(l[3], sizeof(l[0]), "sag    %.0f / %.0f mV  pulse %.0f / %.0f",
             a.sag_n ? a.sag_sum / a.sag_n : 0.0f, b.sag_n ? b.sag_sum / b.sag_n : 0.0f,
             a.pulse_n ? a.pulse_sum / a.pulse_n : 0.0f, b.pulse_n ? b.pulse_sum / b.pulse_n : 0.0f);
    snprintf(l[4], sizeof(l[0]), "full   %.3f / %.3f V rested", a.v_full, b.v_full);
    snprintf(l[5], sizeof(l[0]), "read   tools/curve_fit.py --port");
    snprintf(l[6], sizeof(l[0]), "button 3 s: erase, next battery");
    const char *pl[7] = {l[0], l[1], l[2], l[3], l[4], l[5], l[6]};
    right_lines(e, pl, 7);

    float xmax = a.load_s > b.load_s ? a.load_s / 3600.0f : b.load_s / 3600.0f;
    xmax = xmax < 25 ? 25 : ceilf(xmax * 1.05f / 5) * 5;
    Plot p = {26, 98, 370, 96, xmax, 3.2f, 4.25f};
    frame(e, p, 5, "h", 3.30f);
    if (a.curve.n) plot_curve(e, p, a.curve, true);
    if (b.curve.n) plot_curve(e, p, b.curve, false, b.load_s / 3600.0f, b.v_last);
    legend(e, p, "D2", "D1");
}

static void tables(EPaper &e, const State &s)
{
    const int y = 212;
    // Discharges, left.
    const float ma = s.load_ma();
    tx(e, 2, y, s.load_ma_measured() > 0 ? "DIS  hours   mAh  full sag" : "DIS  hours  ~mAh  full sag");
    e.drawLine(2, y + 9, 196, y + 9, 0);
    for (int k = 1; k <= 2; k++) {
        const Discharge &d = s.d[k];
        char r[80];
        if (!d.present) snprintf(r, sizeof(r), "D%d   --", k);
        else snprintf(r, sizeof(r), "D%d%c %6.2f %5.0f %5.3f %3.0f", k, d.ended ? ' ' : d.voided ? 'x' : '>',
                      (d.q0_h > 0 ? d.q0_h : d.load_s / 3600.0f), d.mah(ma), d.v_full,
                      d.sag_n ? d.sag_sum / d.sag_n : 0.0f);
        tx(e, 2, y + 13 + (k - 1) * 11, r);
    }
    if (s.d[1].q0_h > 0 && s.d[2].q0_h > 0) {
        char r[48];
        snprintf(r, sizeof(r), "D2/D1  %.1f %%", 100.0f * s.d[2].q0_h / s.d[1].q0_h);
        tx(e, 2, y + 35, r);
    }
    // Charges, right.
    e.drawLine(199, y - 2, 199, y + 44, 0);
    tx(e, 204, y, "CHG  time   CC   CV   top  mAh");
    e.drawLine(204, y + 9, 398, y + 9, 0);
    for (int k = 0; k <= 2; k++) {
        const Charge &c = s.c[k];
        char r[160], a[24], b[24], cc[24];
        if (!c.present) snprintf(r, sizeof(r), "C%d   --", k);
        else {
            hm(a, sizeof(a), c.done ? c.t_done : c.t);
            if (c.done) { hm(b, sizeof(b), c.t_cv); hm(cc, sizeof(cc), c.t_done - c.t_cv); }
            else { strcpy(b, " -- "); strcpy(cc, " -- "); }
            // The charge put in, from the R7 readings — only when they cover the whole charge.
            char q[16] = "  --";
            if (c.prog_covers()) snprintf(q, sizeof(q), "%4.0f", c.prog_mah);
            snprintf(r, sizeof(r), "C%d%c %5s %4s %4s %5.3f %s", k, c.done ? ' ' : c.cut ? 'x' : '>', a, b, cc,
                     c.v_top, q);
        }
        tx(e, 204, y + 13 + k * 11, r);
    }
}

void draw(EPaper &e, const State &s, const View &v)
{
    e.clear(0xFF);
    title_bar(e, s, v);
    if (s.complete && s.cur != State::CurCharge) complete_view(e, s, v);
    else if (s.complete && s.charge().done && s.charge().relaxed_s > 1800) complete_view(e, s, v);
    else if (s.cur == State::CurDischarge) discharge_view(e, s, v);
    else if (s.cur == State::CurCharge) charge_view(e, s, v);
    else {
        big_value(e, "--", "", "nothing measured yet", "", "");
        const char *l[7] = {"A test: charge, discharge, charge,",
                            "discharge, charge - about 3.5 days.",
                            "Move the cable when the bar says so:",
                            "the charger can't be switched off,",
                            "and USB takes the cell off the load.",
                            "",
                            "Calibrate the divider first."};
        right_lines(e, l, 7);
    }
    tables(e, s);

    // Footer: what the estimates rest on, and the instruction.
    char note[96];
    const bool charge_view_shown = s.cur == State::CurCharge && !(s.complete && s.charge().done && s.charge().relaxed_s > 1800);
    if (charge_view_shown && v.page)
        snprintf(note, sizeof(note), "page %s  PIN %04d", v.page, v.pin);
    else if (charge_view_shown && !s.charge().done)
        snprintf(note, sizeof(note), "no Wi-Fi: no page - note R7 volts and times");
    else if (s.load_ma_measured() > 0)
        snprintf(note, sizeof(note), "load %.1f mA measured  gain %.4f%s", s.load_ma_measured(), v.gain,
                 v.calibrated ? "" : " NOT CALIBRATED");
    else
        snprintf(note, sizeof(note), "~ = time x %.0f mA (est.)  gain %.4f%s", LOAD_MA, v.gain,
                 v.calibrated ? "" : " NOT CALIBRATED");
    tx(e, 2, 266, note);
    char buf[48];
    const char *al = v.alert ? v.alert : alert_for(s, v, buf, sizeof(buf));
    e.fillRect(0, 278, W, 22, 0);
    ctx(e, W / 2, 281, al, 2, 1);
}

}  // namespace CurveScreen
