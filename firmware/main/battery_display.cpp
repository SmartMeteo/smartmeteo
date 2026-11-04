#include "battery_display.h"
#include <math.h>

namespace BatteryDisplay {

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void reset(State &s)
{
    s.magic = MAGIC;
    s.shown = -1.0f;
    s.tgt_f = -1.0f;
    s.last_t = 0.0;
    s.unplug_t = 0.0;
    s.on_usb = 0;
    s.full_seen = 0;
    s.learn_pending = 0;
}

// RTC_NOINIT survives a restart, and after a new image or a power loss it holds whatever bytes were
// there. Every field is checked, not just the magic: a state this code wrote is always in range.
static bool valid(const State &s)
{
    return s.magic == MAGIC
        && isfinite(s.shown) && s.shown >= -1.0f && s.shown <= 100.0f
        && isfinite(s.tgt_f) && s.tgt_f >= -1.0f && s.tgt_f <= 100.0f
        && isfinite(s.last_t) && isfinite(s.unplug_t)
        && s.on_usb <= 1 && s.full_seen <= 1 && s.learn_pending <= 1;
}

float target(float measured, float k)
{
    const float m = clampf(isfinite(measured) ? measured : 0.0f, 0.0f, 100.0f);
    if (!(k >= 1.0f && k <= K_MAX)) k = 1.0f;
    // Full stretch above FADE_TOP_PCT, none below LOW_PCT. In between, the factor shrinks as the
    // measurement does, and since both shrink together the product still only falls.
    const float f = clampf((m - LOW_PCT) / (FADE_TOP_PCT - LOW_PCT), 0.0f, 1.0f);
    return clampf(m * (1.0f + (k - 1.0f) * f), 0.0f, 100.0f);
}

Step step(State &s, float measured, bool on_usb, bool done, float cable_shown,
          double now_s, float k, bool have_k)
{
    if (!valid(s)) reset(s);
    if (!(k >= 1.0f && k <= K_MAX)) { k = 1.0f; have_k = false; }   // storage held nonsense
    Step out{ s.shown >= 0.0f ? s.shown : 0.0f, false, k };
    // An unreadable cell changes nothing: the last figure stays, and so does the timeline.
    if (!isfinite(measured) || !isfinite(now_s)) return out;

    const float m = clampf(measured, 0.0f, 100.0f);
    float tgt = target(m, k);

    if (on_usb) {
        // The cable's own logic decides what is shown while charging (the charging ceiling, and 100 %
        // once the charger says so). Here it is only remembered, so that the battery side starts
        // from it instead of from the measurement.
        if (done) s.full_seen = 1;
        const float cs = clampf(isfinite(cable_shown) ? cable_shown : m, 0.0f, 100.0f);
        s.on_usb = 1;
        s.learn_pending = 0;
        s.shown = cs;
        s.tgt_f = tgt;
        s.last_t = now_s;
        out.shown = cs;
        return out;
    }

    if (s.on_usb || s.shown < 0.0f) {
        if (s.on_usb && s.full_seen) {
            // Off the cable after a FINISHED charge. It is full, so it says 100, and it keeps saying
            // it while the cell settles: what the voltage does in those hours is relaxation from the
            // charger, not charge leaving.
            s.shown = 100.0f;
            s.tgt_f = tgt;
            s.learn_pending = 1;
            s.unplug_t = now_s;
        } else if (s.shown < 0.0f) {
            s.shown = tgt;          // nothing on the glass to glide from: a first reading, or a new image
        }
        // Otherwise the cable came out before the charge finished, and the charging figure is the
        // one on the glass. It stays as the start of the glide — the estimate it came from is
        // deliberately generous, and dropping to the measurement now is the jump this file is for.
        s.on_usb = 0;
        s.full_seen = 0;
        s.tgt_f = tgt;          // the filter starts where the glass does, never from a stale aim
        s.last_t = now_s;
        out.shown = s.shown;
        return out;
    }

    double dt = now_s - s.last_t;
    // Readings come every gas period, five minutes. Hours between two of them are not time that
    // passed on the battery but the clock being set (the first NTP sync moves it by years), and a
    // clock stepped back is the same thing in the other direction: neither is credited to the glide.
    if (!(dt > 0.0) || dt > MAX_STEP_S) dt = 0.0;

    if (s.learn_pending) {
        const double since = now_s - s.unplug_t;
        if (since >= 0.0 && since < SETTLE_S) {
            s.shown = 100.0f;
            s.tgt_f = tgt;
            s.last_t = now_s;
            out.shown = 100.0f;
            return out;
        }
        s.learn_pending = 0;
        // A clock that ran backwards ends the wait with nothing learned: the timing it rests on is gone.
        if (since >= 0.0 && m >= MIN_FULL_PCT) {
            const float km = clampf(100.0f / (m > 0.0f ? m : 100.0f), 1.0f, K_MAX);
            out.k = clampf(have_k ? k + 0.5f * (km - k) : km, 1.0f, K_MAX);
            out.learned = true;
            k = out.k;
            tgt = target(m, k);    // and the glide absorbs the rescale at the owner's pace
            s.tgt_f = tgt;         // the rescale is a change of units, not something to filter
        }
    }

    // Toward the target at the owner's pace — IN BOTH DIRECTIONS (2026-10-19). Until then the rule
    // was "fall with the measurement, close the gap at that pace, and never up", and the "never up"
    // was a latch, not a limit: the figure could only ever take the lowest value the target had ever
    // reached, and one bad reading kept it there for good.
    //
    // MEASURED ON THE FIELD BOARD, 2026-10-19. 4.0323 V, which this cell's own table (bench f708)
    // calls 74.5 %, against 67 on the glass and in Home Assistant: seven and a half points, stuck
    // for a month. The cause is not noise in the ordinary sense. A console session holds the radio
    // on for tens of minutes at ~90 mA, and 103450's internal resistance measured 0.87 ohm, so the
    // cell reads tens of millivolts low for the whole session; on this plateau one millivolt is
    // 0.83 %, and the smoothing (EMA) follows an excursion that long. The target dived, the latch
    // wrote the dive down as the new ceiling, and the cell has been climbing back toward a figure
    // it was no longer allowed to reach.
    //
    // TWO STEPS, and each answers a different half of the problem.
    //
    //   1. SMOOTH WHAT WE ARE AIMING AT, because the measurement is noisy. The gauge is good to
    //      +-7.5 mV and on this cell's plateau a millivolt is 0.83 %, so the aim jitters by around
    //      a point from one reading to the next even after battery_source's own average.
    //   2. MOVE TOWARD IT AT THE OWNER'S PACE, because the glass must not jump. Half a point an
    //      hour: twenty points in about two days, the figure the owner chose on 2026-10-05.
    //
    // A RATE LIMIT ALONE WILL NOT DO, and the first version of this change was exactly that — it
    // was caught by the noise case in tests/t_cycle.cpp before it ever reached a device. When the
    // jitter between readings is larger than one step of the limit, the limiter moves up a step,
    // down a step, up, down, and NEVER TRACKS THE MEAN: on a measurement falling a point a day
    // under +-0.5 of jitter the glass stood still for forty-two hours. A limiter can only follow a
    // signal it can keep up with, so the signal has to be made followable first. That is step 1,
    // and it has to be an EMA rather than anything one-sided — see below.
    //
    // WHAT WAS HERE UNTIL 2026-10-19 was "fall with the measurement, close the gap at that pace,
    // and never up". The "never up" was a LATCH, not a limit: the figure could only ever take the
    // lowest value the aim had ever reached.
    //
    //   MEASURED ON THE FIELD BOARD, 2026-10-19: 4.0323 V, which this cell's own table (bench f708)
    //   calls 74.5 %, against 67 on the glass and in Home Assistant. Seven and a half points, stuck
    //   for a month, and nothing could ever have brought it back — the cell has to fall to meet it.
    //   One loaded reading is all it takes: a console session holds the radio on for tens of
    //   minutes at ~90 mA, 103450 measured 0.87 ohm, and tens of millivolts here are tens of points.
    //
    // So the one-sided rule is gone from both ends of this file. "It does not climb on the battery"
    // still holds for a reader — at half a point an hour the whole number can gain at most one digit
    // in two hours, and only after the measurement itself has risen — but it is now a LIMIT, and a
    // limit lets the figure come back. The real decline is eighteen times slower than it
    // (0.68 %/day = 0.028 points an hour), so in ordinary running the glass follows the cell with no
    // lag worth naming; only the abnormal is smoothed, which is the whole point.
    //
    // BELOW LOW_PCT THERE IS NO SMOOTHING AT ALL (a = 1) and the pace is four times higher. Two
    // reasons, and the second is the one that matters: the curve is steep down there — 91 mV
    // between 1 % and 2 % against 1.4 mV a point on the plateau — so the noise this filter exists
    // for simply is not present; and the lockout, the history and the 15/10/5 % warnings are all
    // read off the MEASUREMENT, so the glass must agree with it exactly at the end (owner's
    // decision, 2026-10-05). A four-hour filter near the cutoff, where the charge falls fast,
    // would put the glass several points above the cell at the worst possible moment.
    const bool  low   = m < LOW_PCT;
    const float a     = low ? 1.0f : (float)(1.0 - exp(-dt / SMOOTH_TAU_S));
    const float rate  = low ? CATCHUP_LOW_PCT_H : CATCHUP_PCT_H;
    const float limit = (float)(rate * dt / 3600.0);
    if (!(s.tgt_f >= 0.0f)) s.tgt_f = tgt;          // nothing to filter from yet
    s.tgt_f = clampf(s.tgt_f + a * (tgt - s.tgt_f), 0.0f, 100.0f);
    const float nv = clampf(s.shown + clampf(s.tgt_f - s.shown, -limit, limit), 0.0f, 100.0f);

    s.shown = nv;
    s.last_t = now_s;
    out.shown = nv;
    return out;
}

}  // namespace BatteryDisplay
