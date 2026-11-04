#pragma once

#include <stdint.h>
#include "max17048.h"
#include "driver/i2c_master.h"

// Battery monitor built on top of the MAX17048 fuel gauge.
//
// The gauge's CRATE resolution is 0.208 %/h; this device discharges at roughly
// 0.02-0.1 %/h, i.e. below one LSB. So CRATE is only useful to detect CHARGING
// (tens of %/h). The discharge rate and time to empty come from the history ring,
// not from here — see power_estimate.h for why there is only one estimator.

enum BatteryChargeState {
    BAT_DISCHARGING = -1,
    BAT_IDLE        = 0,   // discharge slower than CRATE can resolve
    BAT_CHARGING    = 1,
};

struct BatteryInfo {
    bool     valid = false;
    float    voltage = 0.0f;        // V
    float    soc = 0.0f;            // %, the number the owner is shown (see the charging cap)
    // What the gauge actually said. It differs from `soc` only while the cable is in, and only
    // because the charger is holding the cell's terminals above its resting voltage. The history
    // ring logs THIS one: that ring is a measurement record and the input to characterising the
    // cell, and a number we adjusted for a human to read has no business in it.
    float    soc_raw = 0.0f;        // %
    float    crate = 0.0f;          // %/h (gauge-reported, signed)
    int      charge_state = BAT_IDLE;
    float    ttf_h = -1.0f;         // time to full while charging, hours (-1 = unknown)
    bool     ic_reset = false;      // gauge lost power since last run (battery swap)
    uint8_t  alert_flags = 0;       // MAX17048::STATUS_* latched while MCU slept
    uint32_t cycles = 0;            // accumulated full discharge cycles (persisted in NVS)
};

class BatteryMonitor {
public:
    static BatteryMonitor& getInstance();

    // Configure the gauge and handle battery-swap detection. Call once per
    // boot, right after MAX17048::init(). Performs a quickstart if the gauge
    // reports it lost power (STATUS_RI), reads+clears latched alert flags,
    // enables permanent hibernate and programs alert thresholds.
    // The bus, not a gauge: which chip (if any) answers on it is decided inside, so a v1 and a v2
    // board run the same image. See battery_source.h.
    void begin(i2c_master_bus_handle_t bus);

    // Per-cycle update: reads the gauge and fills `info`. `monotonic_sec` is
    // a clock that keeps counting across deep sleep (the BSEC virtual clock).
    // Call exactly once per wake cycle — it advances the charging ceiling and
    // cycle-counting state in RTC memory.
    // on_usb is the CABLE, read from its own pin. Charge cannot enter this device any other
    // way, so it is the fact that decides whether the gauge's rate-of-change may be read as
    // charging at all — see the note in update().
    void update(i2c_master_bus_handle_t bus, double monotonic_sec, BatteryInfo &info, bool on_usb);

    // Drop the charging ceiling's anchor — "the charge before the cable went in, and when it went
    // in". It lives in RTC_NOINIT so that an update installing itself on the cable does not hand the
    // glass back the charger's inflated number; the price is that a NEW image reads it through a
    // moved layout, where it is whatever bytes the old one left. Zero bytes are 0.0 %, a perfectly
    // "known" anchor, and the device on the charger then shows 0 % plus the minutes since boot —
    // "1 % at 4.15 V", seen on a v2 board flashed over the cable on 2026-09-30. Called from
    // forget_previous_build(), which also runs after a power loss (the build stamp is garbage then).
    // With no anchor the cable shows the uncapped reading until the next unplug re-anchors it.
    void forgetChargeAnchor();

private:
    BatteryMonitor() = default;
    uint8_t boot_alert_flags = 0;
    bool    ic_was_reset = false;
};
