#pragma once
// The cell this device is built around. Set by whoever builds it, not by whoever owns it.
//
// These were settings in the console until 2026-09-27, on the reasoning that a setting whose
// consequences are shown to the owner ought to be correctable by them. Owner's decision that day:
// the wrong call. The cell is soldered to a pack inside a closed enclosure — the owner cannot fit
// a different one without opening it, and if they do, they are the manufacturer for that unit and
// can change this line. What the field actually offered was a way to make the device wrong:
//
//   * capacity feeds the charge-rate ceiling and every "months left" figure. Set it to 3500 on a
//     2000 mAh pack and the device reports a life it does not have, for ever, with no way for the
//     owner to tell that the number is the setting's fault rather than the cell's.
//   * the cutoff is a safety threshold. Lower squeezes out run time by discharging the cell
//     further; it is also the only software guard this board has, and on the v2 board — where the
//     fuel gauge is gone — it will be the only one left.
//
// One header because four places read these and they must not drift: the fuel gauge's VALRT
// registers, the lockout in the cycle, the alert text, and the console's own model.
#include <stdint.h>

// Printed on the pack fitted at build time. The firmware counts on about 85 % of it being usable
// and stops before the cell is empty, so this is the nameplate figure, not the usable one.
static constexpr uint32_t BATTERY_CAPACITY_MAH = 2000;

// Below this the device shows LOW BATTERY and sleeps until a cable arrives. 3.30 V leaves the cell
// with a margin; the pack's own protection circuit sits lower still and is the last resort, not
// this one. Kept inside the 3.0-3.6 V the gauge's VALRT register can express.
static constexpr float BATTERY_CUTOFF_V = 3.30f;

static_assert(BATTERY_CAPACITY_MAH >= 100 && BATTERY_CAPACITY_MAH <= 20000,
              "capacity outside anything this board could be built with");
static_assert(BATTERY_CUTOFF_V >= 3.0f && BATTERY_CUTOFF_V <= 3.6f,
              "cutoff outside what the gauge's VALRT register can hold");
