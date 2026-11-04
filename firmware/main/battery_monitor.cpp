#include "battery_monitor.h"
#include "battery_display.h"
#include "battery_source.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <math.h>

static const char* TAG = "BAT_MON";

// Tunables
static constexpr float CHG_CRATE_MIN      = 2.0f;   // %/h; above this = definitely on charger
static constexpr float DIS_CRATE_MIN      = -0.5f;  // %/h; below this = gauge itself sees discharge
#include "battery_spec.h"
static constexpr float LOWBAT_V_DEFAULT   = BATTERY_CUTOFF_V;

// Long-horizon state that must survive deep sleep (lost only on full power
// loss, in which case begin() re-seeds it anyway via the RI flag).
// The charging cap (see update()). NOINIT so that a restart on the cable — an update installing
// itself, say — does not hand the glass back the charger's inflated number.
RTC_NOINIT_ATTR static float  rtc_rest_soc;      // last reading taken with no charger attached
RTC_NOINIT_ATTR static double rtc_chg_t0;        // when the cable went in, seconds on update()'s clock
// TP4056 with R18 = 1.2 kOhm programs 1 A. The margin is one-way: it must never make the ceiling
// lower than real charging, because then genuine progress would be hidden.
static const float CHG_CURRENT_MA  = 1000.0f;
static const float CHG_RATE_MARGIN = 1.2f;

// The percentage the owner sees (battery_display.h). NOINIT for the same reason as the anchor above:
// an update installing itself must not restart the glide from the measurement. The state carries its
// own magic and range checks, so the bytes a previous image left reset it instead of being read.
RTC_NOINIT_ATTR static BatteryDisplay::State rtc_disp;

RTC_DATA_ATTR static float  rtc_last_soc      = -1.0f;
RTC_DATA_ATTR static float  rtc_discharge_acc = 0.0f;  // % discharged toward the next whole cycle

BatteryMonitor& BatteryMonitor::getInstance() {
    static BatteryMonitor instance;
    return instance;
}

void BatteryMonitor::forgetChargeAnchor() {
    rtc_rest_soc = -1.0f;                // outside 0..100: read as "not known" by update()
    rtc_chg_t0   = 0.0;
    BatteryDisplay::reset(rtc_disp);     // the same moved layout holds the shown figure
}

void BatteryMonitor::begin(i2c_master_bus_handle_t bus) {
    // Everything in this function is the gauge's: its version, its latched alert flags, its
    // quickstart, its VALRT registers. A v2 board has none of them, and there is nothing to put
    // in their place — the divider has no state and no alerts. The lockout does not depend on any
    // of it (battery_too_low reads the voltage directly), so skipping is safe, not a gap.
    //
    // Asked of the revision, not of gauge.init(): init() only adds the device to the bus and asks the
    // chip nothing, so on a v2 board it "succeeded" and the reads below each logged an I2C error about
    // a part that is absent by design — the same trap park() was taken out of on 2026-09-29.
    const BatterySource::Rev rev = bus ? BatterySource::revision(bus) : BatterySource::RevUnknown;
    MAX17048 gauge;
    if (rev != BatterySource::RevGaugeV1 || !gauge.init(bus)) {
        if (rev == BatterySource::RevDividerV2) ESP_LOGW(TAG, "no fuel gauge on this board — running on the divider");
        else                                    ESP_LOGW(TAG, "board not identified — fuel gauge setup skipped");
        return;
    }

    uint16_t version = 0;
    uint8_t id = 0;
    if (gauge.getVersion(version) && gauge.getID(id)) {
        ESP_LOGI(TAG, "MAX17048 version 0x%04X, die ID 0x%02X", version, id);
    }

    uint8_t flags = 0;
    if (gauge.getStatusFlags(flags)) {
        boot_alert_flags = flags;
        ic_was_reset = (flags & MAX17048::STATUS_RI) != 0;

        if (flags & ~MAX17048::STATUS_RI) {
            ESP_LOGW(TAG, "Latched alerts since last clear: 0x%02X [%s%s%s%s%s]", flags,
                     (flags & MAX17048::STATUS_VH) ? "V-high " : "",
                     (flags & MAX17048::STATUS_VL) ? "V-low " : "",
                     (flags & MAX17048::STATUS_VR) ? "V-reset " : "",
                     (flags & MAX17048::STATUS_HD) ? "SOC-low " : "",
                     (flags & MAX17048::STATUS_SC) ? "SOC-change" : "");
        }

        if (ic_was_reset) {
            // Gauge lost power: first power-up or the battery was swapped.
            // Quickstart re-estimates SOC from open-circuit voltage, and the cycle
            // bookkeeping for the old cell is meaningless now.
            ESP_LOGW(TAG, "Gauge reports power loss (battery swap / first boot) -> quickstart");
            gauge.quickstart();
            rtc_last_soc = -1.0f;
            rtc_discharge_acc = 0.0f;
        }
        gauge.clearStatusFlags(0x3F);
    }

    // Built in, not stored: see battery_spec.h. The clamp below stays anyway — it costs nothing
    // and it is what a static_assert cannot do for a value that reaches a hardware register.
    float vlock = BATTERY_CUTOFF_V;
    // A value outside what this cell can be is not a setting, it is damage: it reaches the gauge's
    // VALRT register as well as the lockout test, and a minimum above the maximum is not a state
    // the chip has. Clamped on the way out of storage so a unit that already holds a bad value
    // recovers by itself, without the field ever being editable again.
    if (!(vlock >= 3.0f && vlock <= 3.6f)) vlock = LOWBAT_V_DEFAULT;

    // Permanent hibernate: gauging continues at 45 s cadence (~4 uA) across
    // the MCU's deep sleep. VALRT/ATHD alerts latch into STATUS while we
    // sleep, so the next wake can see "voltage dipped below lockout" events.
    gauge.enableHibernate();
    gauge.setVoltageAlerts(vlock, 4.30f);
    gauge.setSOCAlertThreshold(10);
}

void BatteryMonitor::update(i2c_master_bus_handle_t bus, double monotonic_sec, BatteryInfo &info, bool on_usb) {
    info = BatteryInfo{};
    info.ic_reset = ic_was_reset;
    info.alert_flags = boot_alert_flags;
    NVSManager::getInstance().getU32("bat_cycles", info.cycles, 0);

    // The only place that knows which board this is. Everything below works on volts, percent and
    // rate, whichever revision produced them.
    const BatterySource::Rev rev = BatterySource::revision(bus);
    // v2 only: the charger's own "finished", read once and used three times below — the
    // percentage, the charging ceiling and the charge state must not disagree about it.
    bool v2_done = false;
    bool v1_done = false;      // v1: the same fact, inferred from the gauge (below)
    float model_soc = -1.0f;   // v1: what the gauge's own model says, for the log only
    if (rev == BatterySource::RevGaugeV1) {
        MAX17048 gauge;
        if (!(gauge.init(bus) && gauge.getVCELL(info.voltage) && gauge.getCRATE(info.crate))) {
            ESP_LOGE(TAG, "Fuel gauge read failed");
            return;
        }
        // The percentage comes from THIS cell's measured curve, not from the gauge's model. The
        // model was never configured for the cell (ModelGauge's factory default), and on the
        // 103450's plateau it sat almost still: 1 % in a week on the field board, 2026-10-01..08,
        // while the volts said about five. That froze every estimate built on it — the history's
        // slope, the console's battery card, the hub's "days left" — at "collecting".
        //
        // The gauge stays what it is good at: a voltage with its own reference, measured every 45 s
        // while the MCU sleeps, and a charge rate that resolves charging. Its model is still read,
        // only so the log can show the two side by side.
        if (!gauge.getSOC(model_soc)) model_soc = -1.0f;
        // TP4056's termination window as the gauge sees it (see charge_finished() in
        // bench_screens.cpp): near-full voltage, rate too slow to be charging.
        v1_done = on_usb && info.voltage >= 4.10f && fabsf(info.crate) < 0.5f;
        info.soc = BatterySource::percent(info.voltage, monotonic_sec, on_usb, v1_done,
                                          BatterySource::cellFor(rev));
    } else {
        if (!BatterySource::readVoltage(bus, info.voltage)) {
            ESP_LOGE(TAG, "cell voltage unreadable");
            return;
        }
        // The voltage itself goes out as measured — it is a measurement, and the history keeps it.
        // The percentage is an estimate: the curve over an average across readings (a few
        // millivolts of load recovery are a whole point mid-curve), 100 % when the charger says it
        // has finished, and no rise with the cable out (battery_source.cpp, percent()).
        v2_done = on_usb && BatterySource::chargeComplete(true) == 1;
        info.soc = BatterySource::percent(info.voltage, monotonic_sec, on_usb, v2_done,
                                          BatterySource::cellFor(rev));
        // No gauge, no charge rate, and zero is the honest value rather than a guess: the block
        // below reads it as "slower than this can resolve" and falls through to comparing
        // successive percentages, which is what it already does for a barely-moving cell.
        info.crate = 0.0f;
    }
    info.valid = true;
    if (info.soc > 100.0f) info.soc = 100.0f;
    info.soc_raw = info.soc;
    // The charger's "finished", on either board. v2 asks the chip; v1 infers it the way the screen
    // does (charge_finished() in bench_screens.cpp): near-full voltage with a rate too slow to be
    // charging is TP4056's termination window. Read once: the cable figure, the shown figure and the
    // charge state below must not disagree about it.
    const bool done = (rev == BatterySource::RevDividerV2) ? v2_done : v1_done;

    // ---- what the charger is allowed to add to the number on the glass -------------------------
    // This gauge infers charge from VOLTAGE. With the cable in, TP4056 holds the cell at 4.2 V
    // because that is what a charger does, not because the cell is full — so the gauge reads near
    // 100 %, and the moment the cable comes out the reading falls to whatever was actually in
    // there. The owner sees that as the battery dropping twenty points. It did not: the number
    // was wrong while it was high, and right after.
    //
    // The first half of the fix is to stop the rise: while on the cable the shown value may not
    // exceed what could PHYSICALLY have gone in since the cable went in. TP4056 with R_PROG = 1.2 kOhm
    // (R18 on v1, R7 on v2) gives 1 A, so a 2000 mAh cell cannot gain more than about 50 %/h; the margin below keeps
    // genuine charging from ever being under-reported. It also fixes the worse half of the same
    // problem — a device that said "100 %" over a half-empty cell.
    //
    // The ceiling alone does not abolish the step: it is deliberately a little generous, so it sits
    // above what really went in, and in the charger's falling-current phase by tens of points (the
    // R7 readings of 2026-10-05: a full charge spends half of its time there). Until 2026-10-05 that
    // step was left on purpose — "smoothing the fall shows an inflated figure for longer". The owner
    // decided the other way, and the second half (below, battery_display.h) glides it out instead:
    // within about two days, never under the measurement, and gone entirely below 20 %, where a
    // number that runs ahead of the cell would start to matter.
    float cable_shown = info.soc;
    {
        const float cap_mah = (float)BATTERY_CAPACITY_MAH;
        const float max_rate = 100.0f * CHG_CURRENT_MA / cap_mah * CHG_RATE_MARGIN;   // %/h

        // RTC_NOINIT, not RTC_DATA like the slope above: an update installs itself with a restart,
        // and losing the anchor there would put the inflated reading back on the glass. Garbage is
        // therefore possible and is checked for rather than assumed away.
        const bool rest_known = (rtc_rest_soc >= 0.0f && rtc_rest_soc <= 100.0f);
        if (on_usb) {
            // A window that runs backwards is a broken anchor, not a short one: the clock was
            // stepped (the first NTP sync moves it by years), and a negative window would never grow.
            if (!rest_known || rtc_chg_t0 <= 0.0 || monotonic_sec < rtc_chg_t0) rtc_chg_t0 = monotonic_sec;
            if (rest_known) {
                const double hours = (monotonic_sec - rtc_chg_t0) / 3600.0;
                const float ceiling = rtc_rest_soc + (float)(hours * max_rate);
                // The ceiling bounds an ESTIMATE. When the charger itself says it has finished, the
                // 100 % is a fact and is not capped by a guess about how fast it got there.
                if (cable_shown > ceiling && !done) {
                    ESP_LOGI(TAG, "charging: gauge says %.1f %%, showing %.1f %% "
                                  "(%.0f min on the cable at up to %.0f %%/h from %.1f %%)",
                             cable_shown, ceiling, hours * 60.0, max_rate, rtc_rest_soc);
                    cable_shown = ceiling;
                }
                // Plugging in never takes a point away. The anchor is what the glass showed on the
                // battery, which the stretch below can put above the measurement; charge only goes
                // up from here.
                if (cable_shown < rtc_rest_soc) cable_shown = rtc_rest_soc;
            }
            if (done) cable_shown = 100.0f;   // "100 %" means the charger finished — on both boards
            if (cable_shown > 100.0f) cable_shown = 100.0f;
        }
    }

    // ---- what the owner sees off the cable ----------------------------------------------------
    // Decision of the owner, 2026-10-05: no jump down when the cable comes out, and a glide instead
    // of a step whenever the shown figure is ahead of the measurement. The rules are in
    // battery_display.h; here they get their state and the stretch learned at the last full charge.
    // The ceiling above still limits the rise on the cable — it stops the number running ahead —
    // and this limits the fall after it, so between them the glass never moves faster than charge.
    {
        float k = 1.0f;
        const bool have_k = NVSManager::getInstance().getFloat("bat_fullk", k, 1.0f);
        const BatteryDisplay::Step st = BatteryDisplay::step(rtc_disp, info.soc_raw, on_usb, done,
                                                             cable_shown, monotonic_sec, k, have_k);
        if (st.learned) {
            ESP_LOGI(TAG, "full charge settled at %.1f %% measured: scale %.3f -> %.3f",
                     info.soc_raw, k, st.k);
            if (fabsf(st.k - k) > 0.001f || !have_k) NVSManager::getInstance().setFloat("bat_fullk", st.k);
        }
        if (fabsf(st.shown - info.soc_raw) >= 0.5f)
            ESP_LOGI(TAG, "showing %.1f %% for %.1f %% measured", st.shown, info.soc_raw);
        info.soc = st.shown;
    }
    if (!on_usb) {
        // No charger on the terminals: this is where the next charge starts from, and it starts from
        // what the owner was shown, so that plugging in continues the number instead of resetting it.
        rtc_rest_soc = info.soc;
        rtc_chg_t0   = 0.0;
    }

    // Charge state. CRATE resolves charging easily (tens of %/h) but our
    // discharge is sub-LSB, so "idle" just means "discharging too slowly for
    // the gauge to see"; the real rate comes from the history (power_estimate.h).
    //
    // CHARGING ALSO REQUIRES THE CABLE, and that is not belt-and-braces. This gauge is a model,
    // not a coulomb counter: after a power cycle or a quickstart it re-anchors and reports tens of
    // percent per hour with nothing connected — +17.89 %/h was measured once, +3.3 %/h the day
    // this check was added, on a device sitting on battery. The same trap was found in 2026-07-11
    // and fixed for on_wire; this reader was left believing it.
    //
    // A false "charging" used to cost more than a wrong word on the card: it re-anchored the RTC
    // discharge slope on every wake, so that measurement never accumulated. That slope is gone;
    // the history-based one goes by the cable bit, not by this state. The word is still the
    // owner's to read, and still worth getting right.
    //
    // The asymmetry decides the direction: charge enters this device through one cable and no
    // other way, so requiring the pin cannot hide a real charge. Missing one would be harmless
    // anyway (the charger does its work regardless); inventing one costs the owner every number
    // on the card.
    if (on_usb && info.crate > CHG_CRATE_MIN) {
        info.charge_state = BAT_CHARGING;
    } else if (info.crate < DIS_CRATE_MIN) {
        info.charge_state = BAT_DISCHARGING;
    } else if (rtc_last_soc >= 0.0f && info.soc < rtc_last_soc - 0.05f) {
        info.charge_state = BAT_DISCHARGING;
    } else {
        info.charge_state = BAT_IDLE;
    }

    // v2 has no charge rate to reason from, but it has something better: the charger's own
    // STDBY# line. Asking the chip beats inferring from a number we do not have, and it beats the
    // v1 heuristic too — that one calls the charge finished at 4.10 V with a slow rate, which is
    // a guess about a thing the hardware knows for certain.
    if (rev == BatterySource::RevDividerV2 && on_usb) {
        info.charge_state = v2_done ? BAT_IDLE : BAT_CHARGING;
    }

    // No discharge slope here any more. There was one, over an anchor kept in RTC_DATA — which
    // every restart wipes, every update installed over the air included — and it spoke after six
    // hours whatever the gauge had moved: "0.16 %/day, 523 days" on a device using ~0.6. The rate
    // now comes from the history ring, which survives restarts (power_estimate.h), and the card
    // has one estimator instead of two that disagreed.

    if (info.charge_state == BAT_CHARGING && info.crate > 0.1f) {
        info.ttf_h = (100.0f - info.soc) / info.crate;
    }

    // Cycle counting: accumulate discharged percent; every 100% = one full
    // cycle. NVS is written only on the (rare) whole-cycle increment.
    if (rtc_last_soc >= 0.0f && info.soc < rtc_last_soc) {
        rtc_discharge_acc += rtc_last_soc - info.soc;
        if (rtc_discharge_acc >= 100.0f) {
            rtc_discharge_acc -= 100.0f;
            info.cycles++;
            NVSManager::getInstance().setU32("bat_cycles", info.cycles);
            ESP_LOGI(TAG, "Battery completed full discharge cycle #%u", (unsigned)info.cycles);
        }
    }
    rtc_last_soc = info.soc;

    // The low-battery lockout is decided in one place: battery_too_low() in bench_screens.cpp.

    ESP_LOGI(TAG, "V=%.3fV SOC=%.1f%% CRATE=%+.2f%%/h state=%d TTF=%.1fh cycles=%u",
             info.voltage, info.soc, info.crate, info.charge_state,
             info.ttf_h, (unsigned)info.cycles);
    if (model_soc >= 0.0f)
        ESP_LOGI(TAG, "cell curve says %.1f %%, the gauge's own model %.1f %%", info.soc_raw, model_soc);
}
