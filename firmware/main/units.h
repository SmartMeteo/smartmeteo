#pragma once
// The units a device shows when nobody has told it otherwise — NVS "u_temp" and "u_pres".
//
// One header because THREE readers must agree: fmt_pres(), which writes the Telegram and MQTT
// texts; the dashboard builder, which writes the glass; and /api/status, which is where the
// console's unit selector gets its position from. A default that disagrees between them puts one
// unit on the glass and another on the page, and the owner has no way to tell which the device
// actually believes. That is the shape of defect effWifi() had on 2026-09-30 — the console printing
// a radio interval the firmware would never use — and it is cheap to make impossible here.
//
// Readings are always STORED in Celsius and hPa; these touch display only, which is what keeps the
// history and the API comparable between devices that were set up differently.
#include <stdint.h>

static constexpr uint8_t UNIT_PRES_HPA  = 0;
static constexpr uint8_t UNIT_PRES_MMHG = 1;
static constexpr uint8_t UNIT_TEMP_C    = 0;
static constexpr uint8_t UNIT_TEMP_F    = 1;

// Owner's decision 2026-10-02: millimetres of mercury.
//
// This changes what an EXISTING device shows. "u_pres" is only written when the owner saves the
// units card, so a device that was never told sits on this default and will switch unit on the
// next update — which is the point of changing a default, but it is a visible change and not a
// silent one.
static constexpr uint8_t UNIT_PRES_DEFAULT = UNIT_PRES_MMHG;
static constexpr uint8_t UNIT_TEMP_DEFAULT = UNIT_TEMP_C;

static_assert(UNIT_PRES_DEFAULT <= 1 && UNIT_TEMP_DEFAULT <= 1,
              "the units are one bit each; /api/save stores anything non-zero as 1");
