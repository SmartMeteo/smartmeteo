#include "power_history.h"
#include "battery_spec.h"
#include "hist_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <vector>

namespace PowerHistory {

// What the history hands the fit: every record of the window in time order, thinned on the fly to
// at most MAX_POINTS so a ten-day window at a 5-minute step (2880 records) costs the same 10 KB of
// heap as the 600 records it replaced. Thinning halves the kept points whenever they reach the cap
// and doubles the stride, so the result is evenly spaced whatever the window held.
//
// The CABLE BIT IS NEVER THINNED AWAY. A skipped record that was on the cable passes its bit to the
// next point kept; otherwise a charge falling between two kept points would vanish, and the fit —
// which decides "on battery" by that bit alone — would draw one slope straight across it.
//
// ONE INSTRUMENT PER RUN. A record whose percentage was read a different way from the record before
// it (HIST_FLAG_CURVE, hist_store.h) starts the collection over: everything older is dropped. On the
// v1 board the update of 2026-10-08 switched from the fuel gauge's model to the cell's measured
// curve, and the ten days before it are the gauge's — 0.2 %/day where the curve says 0.75. Sloping
// across both would put a confident, wrong figure on the card for the five days it took the new
// records to fill the tail half. Nothing is lost that was worth keeping: the old records still chart.
struct Collect {
    std::vector<PowerEstimate::Point> pts;
    uint32_t stride = 1, seen = 0;
    bool usb_since_kept = false;
    PowerEstimate::Point last{};
    bool have_last = false, last_kept = false;
    bool have_curve = false, curve = false;
    int  restarts = 0;
};

static void collect(const HistRec &r, void *ctx)
{
    Collect &c = *(Collect *)ctx;
    // Records older than this measurement's own format are skipped, not guessed at. They have no
    // cell voltage and no "was on USB" bit, so a stretch spent charging looks exactly like a
    // stretch spent discharging — which is how a device that has sat on the cable all week can be
    // credited with hours of battery use it never had.
    if (r.vcell_mv == 0) return;
    const bool curve = (r.flags & HIST_FLAG_CURVE) != 0;
    if (c.have_curve && curve != c.curve) {
        c.pts.clear();
        c.stride = 1; c.seen = 0;
        c.usb_since_kept = false;
        c.have_last = c.last_kept = false;
        c.restarts++;
    }
    c.have_curve = true; c.curve = curve;
    // soc_d is the precise field; records written before it existed carry 0 and fall back to whole
    // percent, which is too coarse to slope but better than dropping them.
    PowerEstimate::Point p{ r.t, r.soc_d ? r.soc_d / 100.0f : (float)r.soc, r.vcell_mv / 1000.0f,
                            (r.flags & 0x20) != 0 };
    c.usb_since_kept = c.usb_since_kept || p.usb;
    c.last = p; c.have_last = true;
    if (c.seen++ % c.stride != 0) { c.last_kept = false; return; }

    p.usb = c.usb_since_kept;
    c.usb_since_kept = false;
    c.pts.push_back(p);
    c.last_kept = true;
    if ((int)c.pts.size() >= PowerEstimate::MAX_POINTS) {
        // Keep every second point, the newer of each pair, carrying the older one's cable bit
        // forward — the same direction the bit travels between kept points.
        size_t j = 0;
        for (size_t i = 1; i < c.pts.size(); i += 2) {
            PowerEstimate::Point q = c.pts[i];
            q.usb = q.usb || c.pts[i - 1].usb;
            c.pts[j++] = q;
        }
        c.pts.resize(j);
        c.stride *= 2;
    }
}

PowerEstimate::Result estimate()
{
    static SemaphoreHandle_t lock = nullptr;
    static PowerEstimate::Result cached;
    static uint32_t cached_at = 0;
    if (!lock) lock = xSemaphoreCreateMutex();           // first caller is the loop, before httpd
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(2000)) != pdTRUE) return cached;

    const uint32_t nowms = esp_log_timestamp();
    if (!cached_at || nowms - cached_at >= 30000) {
        HistStore &h = HistStore::getInstance();
        h.init();
        // The window hangs from the newest RECORD, not from the clock: the estimate describes what
        // the history holds, and a clock that is wrong — or a device that was off — does not move it.
        const uint32_t newest = h.newestTimestamp();
        Collect c;
        c.pts.reserve(PowerEstimate::MAX_POINTS);
        if (newest) {
            const uint32_t from = newest > PowerEstimate::WINDOW_S ? newest - PowerEstimate::WINDOW_S : 1;
            // max_out large enough that query() never thins on its own — its stride would drop the
            // cable bit with the record. 4096 is ten days at the shortest step with room to spare.
            h.query(from, 0, 4096, collect, &c);
            if (c.restarts)
                ESP_LOGI("POWER", "power estimate: the percentage was read another way before the "
                                  "newest part of the history — sloping only that part");
            if (c.have_last && !c.last_kept) {
                PowerEstimate::Point p = c.last;
                p.usb = c.usb_since_kept;
                c.pts.push_back(p);
            }
        }
        cached = PowerEstimate::fit(c.pts.data(), (int)c.pts.size(), BATTERY_CAPACITY_MAH);
        cached_at = nowms ? nowms : 1;
    }
    const PowerEstimate::Result r = cached;
    xSemaphoreGive(lock);
    return r;
}

const char *quality(const PowerEstimate::Result &r)
{
    if (!r.valid || r.conf < 1) return "collecting";
    return r.conf >= 2 ? "good" : "rough";
}

}  // namespace PowerHistory
