#include "power_estimate.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "POWER";

namespace PowerEstimate {

Result fit(const Point *pts, int n, uint32_t cap_mah) {
    Result e;
    if (!pts || n < 4 || cap_mah == 0) return e;

    // Newest unbroken run on battery.
    int end = n - 1;
    while (end >= 0 && pts[end].usb) end--;          // skip a charging tail
    if (end < 3) return e;
    int start = end;
    while (start > 0 && !pts[start - 1].usb) start--;

    // A run that begins at a cable begins with a cell still settling off the charger (SETTLE_S,
    // header): its volts — and so a percentage read from them — fall for hours with almost no
    // charge leaving. A run that begins at the start of the data did not come off a cable we saw.
    if (start > 0) {
        const int run_start = start;
        while (start < end && pts[start].t - pts[run_start].t < SETTLE_S) start++;
        if (start != run_start)
            ESP_LOGI(TAG, "power estimate: skipping the first %d point(s) after the cable — the "
                          "cell was still settling", start - run_start);
    }
    if (end - start < 3) {
        ESP_LOGI(TAG, "power estimate: not enough of this run is past the charger yet");
        return e;
    }

    const float span_h = (pts[end].t - pts[start].t) / 3600.0f;
    if (span_h < 1.0f) return e;

    // ONE LEAST-SQUARES LINE OVER THE WHOLE POST-SETTLE RUN, and a second pass that drops the points
    // the first one shows to be outliers — SYMMETRICALLY, by how far they sit from the line, with no
    // opinion about which way.
    //
    // WHAT WAS HERE UNTIL 2026-10-19, and why removing it is the fix rather than a simplification.
    // The run was filtered first: a rise over half a percent between neighbours was subtracted from
    // everything after it as "the estimate re-anchoring", while a fall of the same size was kept as
    // charge that left. That asymmetry was written for a percentage that really did step — the
    // no-rise hold lived in RTC_DATA and every restart re-initialised it — and that hold was taken
    // out of percent() on 2026-10-14. The filter outlived its reason and became the defect:
    //
    //   on the field board's own 26 days, through this file, on data whose true slope three
    //   independent methods put at 0.65-0.71 %/day, it answered 2.05 %/day over ten days and
    //   1.41 over twenty-six — and its own log said why: "9 steps totalling +8.06 % taken out",
    //   "17 steps totalling +14.93 %". The cell gave up 25 points in that month; the filter
    //   invented another 15 and the fit swallowed them. conf came back 2, "trustworthy".
    //
    // The cause is not the threshold. On this cell's plateau one millivolt is 0.83 % and the gauge
    // is accurate to +-7.5 mV, so ordinary noise IS a "rise over half a percent" several times a
    // day; cut every one of them and keep the falls, and noise becomes drift. A median of three
    // neighbours was added on 2026-10-14 against exactly this and was not enough: it removes a
    // one-sample excursion, and a radio window moves two or three records in a row.
    //
    // Least squares does not need protecting from symmetric noise — that is what it averages. It
    // needs protecting from a one-sided hand on the scale, which is what was there.
    double slope = 0.0, icept = 0.0, sigma = 0.0;
    int used = 0;
    for (int pass = 0; pass < 2; pass++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int k = 0;
        for (int i = start; i <= end; i++) {
            const double x = (pts[i].t - pts[start].t) / 3600.0;   // hours
            const double y = pts[i].soc;
            if (pass && sigma > 0.0 && fabs(y - (slope * x + icept)) > OUTLIER_SIGMA * sigma) continue;
            sx += x; sy += y; sxx += x * x; sxy += x * y; k++;
        }
        if (k < 3) return e;
        const double den = k * sxx - sx * sx;
        if (den <= 0) return e;
        const double sl = (k * sxy - sx * sy) / den;
        const double ic = (sy - sl * sx) / k;
        double ss = 0;
        for (int i = start; i <= end; i++) {
            const double x = (pts[i].t - pts[start].t) / 3600.0;
            if (pass && sigma > 0.0 && fabs(pts[i].soc - (slope * x + icept)) > OUTLIER_SIGMA * sigma) continue;
            const double r = pts[i].soc - (sl * x + ic);
            ss += r * r;
        }
        if (!pass) { e.dropped = 0; }
        else       { e.dropped = (end - start + 1) - k; }
        slope = sl; icept = ic; used = k;
        sigma = (k > 2) ? sqrt(ss / (k - 2)) : 0.0;
    }
    if (e.dropped)
        ESP_LOGI(TAG, "power estimate: %d of %d point(s) sat further than %.0f sigma from the line "
                      "and were left out", e.dropped, end - start + 1, (double)OUTLIER_SIGMA);

    // The facts that describe the estimate describe the window it was FITTED over, which is now the
    // whole run past the settling — the tail-half rule went with the filter above. It was there to
    // keep the relaxation out, and SETTLE_S does that by time; halving the window on top of it cost
    // a slow cell the very span in which its movement rises above the gauge's own accuracy.
    e.hours = (float)((pts[end].t - pts[start].t) / 3600.0);
    e.mv    = (pts[start].v - pts[end].v) * 1000.0f;

    if (slope >= 0) return e;                                 // charging or noise-dominated

    // The drop THE FIT ITSELF says across its window, not the gap between its two end samples. The
    // card's three facts hang on this number (MIN_DROP_PCT, header), and two samples are the noisiest
    // reading of a one-percent movement obtainable: on the field board's own ten days the end-to-end
    // gap came out NEGATIVE while every one of the 240 points between them sloped down. Gating a
    // least-squares answer on two of its inputs is the same mistake in a new place.
    e.drop = (float)(-slope * e.hours);
    if (e.drop < MIN_FIT_PCT) return e;                       // the window did not move at all

    e.ua      = (float)(-slope * cap_mah * 10.0);             // %/h * mAh / 100 * 1000
    e.pct_day = (float)(-slope * 24.0);
    // Usable capacity: the whole of it. Zero percent on both cell tables is already the lockout
    // ({BATTERY_CUTOFF_V, 0 %}, battery_source.cpp), so the scale has the unusable tail removed
    // once, and the discharge bench measured 1.99 A.h to 3.30 V on a 2000 mA.h cell (journal
    // 2026-10-06) — nameplate IS the to-cutoff capacity here. Until 2026-10-19 a second 0.85 was
    // taken off on top, which predicted the device dying at 15 % and made "Life at that draw" 15 %
    // shorter than the 100 % / (%/day) printed beside it. Owner's decision, 2026-10-19: one
    // discount, not two, here and in the console's model and on the site.
    e.months = (e.ua > 0) ? (cap_mah / (e.ua / 1000.0f)) / 24.0f / 30.44f : 0;
    e.conf  = (e.drop < MIN_DROP_PCT) ? 0 : (e.drop < GOOD_DROP_PCT) ? 1 : 2;
    e.valid = true;
    (void)used;
    return e;
}

Card card(const Result &r, float soc_now, bool on_usb) {
    Card c{0.0f, 0.0f, -1.0f};
    if (!r.valid || r.conf < 1 || r.pct_day <= 0.0f) return c;
    c.pct_day = r.pct_day;
    c.mah_day = r.ua * 24.0f / 1000.0f;
    if (!on_usb && soc_now > 0.0f) c.tte_h = soc_now / (r.pct_day / 24.0f);
    return c;
}

}  // namespace PowerEstimate
