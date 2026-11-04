// ============================================================================
// SETUP CYCLE — the panel, the access point, the web console and deep sleep.
//
//   press the button:
//     * Wi-Fi NOT configured yet -> AP up, web console up, QR SETUP on the glass,
//                                   3 minutes to complete the wizard
//     * Wi-Fi already configured -> AP up, web console up, QR SETUP on the glass,
//                                   3 minutes to change anything
//   then, either way: radio down -> LOGO back on the glass -> deep sleep.
//
// The window restarts on every web request, so someone filling in the wizard is never cut off;
// a short press ends it early. The LED answers every press: two blinks to acknowledge it, lit
// while the device is working, out just before it sleeps.
//
// Grown one piece at a time from the screens-only bench that worked: pictures, then sleep, then
// the AP, then the button and the LED, and now the web console and the 3-minute window. No
// sensors and no dashboard yet — those are the next pieces.
//
// Everything is logged as FACT, not intent: how long a refresh really took, how much heap is
// free, what woke us. A 4-gray refresh takes about 7 s; anything far below that means the data
// never reached the panel (which is what heap pressure from Wi-Fi does, silently).
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "fw_version.h"
#include "esp_ota_ops.h"   // reverting to the other slot when this image cannot finish a boot
#include "esp_app_desc.h"   // esp_app_get_description(): version + ELF hash
#include "esp_core_dump.h"  // erasing the dump on factory reset: it holds the owner's SSID
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "soc/rtc.h"        // which slow clock is REALLY running
#include "esp_clk_tree.h"   // LP-domain pad control: the pull that survives deep sleep

#include "bench_screens.h"
#include "epaper.h"
#include "pm_gate.h"
#include "portal_art.h"
#include "logo4g.h"     // picture A: the Smart Meteo logo
#include "portal_screen.h" // picture B: the Wi-Fi setup screen, with this device's QR code and password
#include "lock4g.h"     // picture C: the low-battery lock screen
#include "web_server.h"
#include "captive_dns.h"
#include "mdns.h"
#include "nvs_manager.h"
#include "wifi_manager.h"
#include "esp_heap_caps.h"
#include "i2c_manager.h"
#include "scd41.h"
#include "bme690.h"
#include "bsec_manager.h"
#include "nvs.h"
#include "max17048.h"
#include "battery_monitor.h"
#include "battery_source.h"
#include "battery_spec.h"
#include "co2_alert.h"
#include "units.h"
#include "clock_sync.h"
#include "weather.h"
#include "analytics.h"
#include "telegram.h"
#include <functional>
#include "mqtt_publisher.h"
#include "ota_check.h"
#include "ota_install.h"
#include "ArduinoJson.h"
#include "hist_store.h"
#include "evt_log.h"
#include "screen.h"
#include "sun_moon.h"
#include "matter_mode.h"
#include "freertos/semphr.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "SETUP_CYCLE";

#define PORTAL_SEC        180       // how long the setup window stays open with no activity
// The clock tick. Kept at a minute by choice, with the cost written down rather than forgotten.
//
// Measured over 141.7 h on hardware: 6560 partial refreshes against 105 full ones, one update every
// 76 seconds. Good Display asks for no more than one per 180 s on this panel. The owner's Arduino
// device, which has run this same panel in this same room with a clean screen for months, ticks
// every five minutes and so obeys that rule; we do not, and that is the one dimension on which we
// still differ from it (4.70).
//
// Going to 300 was tried and reverted deliberately: the live clock is a feature, and the residue
// is being attacked from the other side first — a faster full refresh, which flashes little enough
// to be run often, and a cleaning frame at night where there was none. If those are not enough,
// the interval is what is left, and this comment is the reason to come back to it.
#define TICK_SEC           60
// WHEN THE PANEL LOSES ITS SUPPLY — settled 2026-09-08, and the answer is "almost never".
//
// The switch itself is harmless. Pulling the battery out of a finished device does not touch the
// image, and the vendor's own reference sequence ends with deep sleep followed by POWER OFF. What
// costs is the TRANSITION. This board has no bleed resistor and no quick output discharge on
// 3.3V_ePaper, so when Q4 opens the few microfarads on the rail drain only through the sleeping
// panel's own 2-5 uA: seconds spent in the region where the controller is neither running nor off.
// One such crossing is free. Fourteen hundred a day is not, and one night of it greyed the
// background — the frame lines and labels, pixels no partial update ever drives, which is the
// proof that this happens BETWEEN frames and not during one.
//
// So the rule is about how often, not about whether:
//
//   RUNNING  — the supply STAYS ON through every sleep. A tick every minute means 1440 crossings a
//              day to save about 1 uA out of some 330. GxEPD2 has no concept of a panel power
//              switch at all, and Watchy redraws e-paper every sixty seconds on a battery without
//              one; both are this exact duty cycle.
//   PARKED   — the supply is CUT, because the device is going away and the picture is final. The
//              logo after a factory reset can sit in a box for months on one crossing, and that is
//              precisely the case the switch was put on the board for. Same for the low-battery
//              notice: drawn once, then hourly wakes that only look for a charger (owner, 2026-09-12).
//
// The parked state has to STAY parked, which is what rtc_panel_powered is for: a wake that draws
// nothing must not turn the panel back on, or a month in a box becomes a crossing every hour
// instead of one. Safe mode deliberately keeps its supply — it redraws every hour, so by this same
// rule it is a running state wearing a static picture, and cutting there would buy nothing.
// How long the controller gets between the sleep command and losing its supply. 2 s is the
// short end of what the panel's maker asked for.
#define PANEL_SETTLE_MS 2000
// How long the switched rail needs to be gone before a wake pin may be armed. The decay was
// measured at about two seconds; the button line is capacitively close enough to hear it.
#define RAIL_QUIET_MS   3000
enum class PanelSleep { Powered, Parked };
// The gas sensor's cadence is NOT a user setting. BSEC's ULP configuration is built around a
// 300-second rhythm, and feeding it anything else stops accuracy from ever accumulating
// (CLAUDE.md 4.6). The Mode tab's interval governs the CO2 sensor, which has no such constraint.
#define GAS_SEC           300
// A minute tick can land a second or two short of the gas period: the stamp is whole seconds and wake
// latency jitters. Without this, such a tick slips a whole minute (20 % late; Bosch allows 6.25 %, AN011
// 3.2). Kept small because BSEC schedules from the call time: a tick N s early waits N s for next_call in
// every later period too, and a two-step heater profile (~3 s) must still fit the 12 s loop.
#define GAS_EARLY_SEC     2
// The 3-hour forecast grid barely moves, but each fetch is a second HTTPS request with its own TLS
// handshake and a 16 KB response. Pulling it every time weather refreshes doubled the cost of a
// radio window for nothing. Today's high/low still track "now" on every refresh through the
// observed temperature; the forecast only has to supply the part of the day still ahead.
#define FORECAST_SEC    10800       // 3 hours
// How fresh the outdoor data must be for a button press to leave the radio alone. OpenWeatherMap
// advises no more than one call per location every ten minutes and refreshes at about that rate,
// so anything younger than this is the same numbers we already have — and raising the radio to
// fetch them again is the most expensive thing a press could do.
#define WX_FRESH_SEC      900       // 15 minutes
// Partial updates leave a faint trace of what they replaced, and it builds up until the whole
// panel reads grey. Only a full refresh clears it.
//
// This used to be counted in wakes (120 x 60 s = 2 h), which was true only while every wake was a
// minute apart. Once the sleep length started following the profile, a wake became 300 s at night
// and the same 120 wakes stretched to TEN HOURS — so a night ran end to end without a single full
// refresh, and the screen was grey by morning. Time is what matters here, so time is what is
// measured; the partial count is a second guard for before the clock is ever set.
// ⚠ DIAGNOSTIC SETTING, deliberately breaking the vendor's rule. Not a configuration choice.
//
// Good Display asks for a full refresh after every 5 partials. Obeying that while the partial path
// is the thing under suspicion has a side effect: the cleaning frame MASKS what is being judged.
// Every five minutes it wipes whatever accumulated, so the glass cannot say whether the vendor
// driver improved the partial update or was merely cleaned often enough to hide it.
//
// So the interval is being walked outwards, one step at a time, until something breaks or nothing
// does:
//   5 partials  (~5 min)  — the vendor's own number, shipped in 0.9.16.
//   1 hour      (~50 partials by day, 12 by night) — 0.9.17. Ran 22 h including a full night;
//                the screen stayed clean. That is the first long stretch in this whole saga with
//                RARE cleaning that did not degrade, and rare cleaning is precisely what used to
//                expose the fault.
//   3 hours     (~180 partials by day, 36 by night) — here. Thirty-six times the vendor's limit.
//
// THE EXPERIMENT IS OVER — back to one hour (owner's decision 2026-09-30). Three hours held without
// ghosting, so the question it was asked to answer is answered: the partial path is sound and the
// old ghosting belonged to the driver that was deleted. Knowing that, the interval is no longer a
// probe but a product setting, and an hour is the one that ran 22 hours clean in 0.9.17 while
// staying far closer to what Good Display asks for.
//
// The cost, from measured numbers rather than a guess: a cleaning frame is 1929 ms against 677 ms
// for a partial, so each one costs 1.25 s extra. Eight a day becomes twenty-four — 20 s a day of
// panel time, 0.22 mAh, 1.6 % of the measured 13.66 mAh/day budget, about two days off the life.
//
// Still twelve times the vendor's own limit (a full frame every 5 partials), and that is deliberate:
// their number is written for panels refreshed continuously, and this one sleeps between frames.
//
// Driven by the WALL CLOCK, not the count, because the same count means one hour by day and five at
// night. The count is the fallback for a device with no clock, set to match.
#define FULL_EVERY_SEC   3600       // one hour between cleaning frames, day and night
#define FULL_EVERY_PARTIALS 60      // fallback without a clock: one hour of minute ticks

// The panel's working temperature. Its datasheet allows 0..+50 C; the lower end is kept a couple of
// degrees inside that, because the reading is the room's rather than the film's own and they differ
// by a degree or two near a window. Outside this band the controller has no waveform that finishes.
#define INK_MIN_C  2.0f
#define INK_MAX_C  50.0f
#define EPD_BUSY_PIN      GPIO_NUM_18
#define PIN_USER_BTN      GPIO_NUM_2    // LP-capable, so it can wake the chip from deep sleep
#define PIN_LED           GPIO_NUM_11
#define PIN_USB_DETECT    GPIO_NUM_3    // low = cable in; honest on this board revision
// The setup screen's QR code is made from the same name and password the access point uses.
#define BENCH_AP_SSID     PORTAL_AP_SSID

// Cycle bookkeeping. RTC_NOINIT survives deep sleep AND esp_restart (the wizard ends with one);
// only a real power loss clears it, which the magic below catches.
#define CYC_MAGIC 0x5C0FFEE1
RTC_NOINIT_ATTR static uint32_t rtc_ls_notes;      // light-sleep notes written by this image
RTC_NOINIT_ATTR static uint32_t rtc_pre_notes;     // wake-prologue notes written by this image
RTC_NOINIT_ATTR static uint32_t rtc_magic;

// Does a reading in RTC belong to THIS build? Every variable moves on an over-the-air update, so
// a field that "survived" is really some other variable's old bytes — and the dangerous ones are
// those that land on a plausible value instead of an obvious absurdity. The marker above is what
// vouches for the layout.
//
// start_cycle_from_scratch() clears these for the working cycle and for Matter. The unconfigured
// path never calls it, which is how an ambient pressure of 1.47e21 Pa reached the CO2 sensor on a
// freshly flashed board (seen in the log, 2026-09-29). The range check inside scd41.cpp caught
// that one — but it only rejects the absurd, and the run-up it feeds ends in a FORCED
// RECALIBRATION, which writes a baseline into the sensor's own EEPROM and keeps it through a
// factory reset. A wrong-but-plausible pressure there would be baked in for good.
static inline bool rtc_readings_are_ours(void) { return rtc_magic == CYC_MAGIC; }
RTC_NOINIT_ATTR static uint32_t rtc_tick;          // wake counter, drives the 5-minute cadence
RTC_NOINIT_ATTR static int64_t  rtc_bsec_ns;       // BSEC's virtual clock, advanced over sleep too
RTC_NOINIT_ATTR static int64_t  rtc_bsec_wall_us;  // wall clock (us) that rtc_bsec_ns corresponds to
RTC_NOINIT_ATTR static time_t   rtc_last_weather;  // last SUCCESSFUL fetch (drives the freshness tag)
RTC_NOINIT_ATTR static time_t   rtc_last_wx_try;   // last ATTEMPT — schedules the radio window
RTC_NOINIT_ATTR static time_t   rtc_last_fc;       // last forecast pull
// MET Norway's Expires for the last answer: its terms ask for no request before it.
RTC_NOINIT_ATTR static time_t   rtc_met_expires;
RTC_NOINIT_ATTR static time_t   rtc_last_meas;    // when the gas sensor last ran (fixed 300 s)
RTC_NOINIT_ATTR static time_t   rtc_last_co2;     // when the CO2 sensor last ran (profile interval)
// When the history ring last got a record. Its step is a setting now (NVS "h_step", 5/10/15 min,
// owner 2026-09-16), no longer the gas period by accident.
RTC_NOINIT_ATTR static time_t   rtc_last_hist;
// A console session asked for from Matter mode: the next boot runs the Wi-Fi console once and then
// returns to Matter. RTC, because the request crosses an esp_restart().
RTC_NOINIT_ATTR static uint32_t rtc_matter_console;
#define MATTER_CONSOLE_MAGIC 0xC0501E5Au
// Set when a scheduled network window failed to connect. Only a FAILURE is worth showing: the
// radio is off almost all the time by design, so a permanent Wi-Fi icon would say nothing.
RTC_NOINIT_ATTR static bool     rtc_wifi_fail;
// How many consecutive joins have failed, and how many scheduled wakes to sit out before trying
// again. Counted in WAKES rather than seconds deliberately: this is the one path where the clock
// may never have been set at all, because NTP needs the very network that is refusing to answer.
RTC_NOINIT_ATTR static uint8_t  rtc_wifi_fails;
RTC_NOINIT_ATTR static uint16_t rtc_wifi_skip;
RTC_NOINIT_ATTR static float    rtc_pres_pa;       // last good pressure, for CO2 compensation
// Last readings, so a clock-only tick can redraw the screen without waking the sensors.
RTC_NOINIT_ATTR static uint16_t rtc_co2;
// Consecutive CO2 read failures. The screen has always had an "SCD41 is broken" state (4.14: hero
// "---", climate "--", an inverted "Error" in the air block, scale arrow hidden) and the renderer
// has always had the branches for it — but NOTHING in the firmware ever set the flag, so the
// branches were unreachable code. What the glass showed instead of that state was a hero reading
// "---" beside a confident computed verdict, the last good temperature and humidity presented as
// current, and the scale arrow parked in the white "good" third of the pill (atoi("---") is 0,
// clamped up to 400). Every one of those is a wrong statement, and this unit's SCD41 has in fact
// failed once (4.23). Three misses in a row is a fault, not a hiccup; one is not worth alarming
// over, and the freshness tag already ages visibly meanwhile.
RTC_NOINIT_ATTR static uint8_t rtc_co2_fails;
RTC_NOINIT_ATTR static float    rtc_temp, rtc_hum, rtc_iaq, rtc_soc;
// What the gauge said, before the charging cap adjusted it for a human to read (battery_monitor.h).
// The history ring logs this one: it is a measurement record and the raw material for working out
// this cell's discharge curve, and the curve is what plate v2 will need once the gauge is gone.
RTC_NOINIT_ATTR static float    rtc_soc_raw;
RTC_NOINIT_ATTR static uint8_t  rtc_acc;
// The web console asks for a good deal more than the glass shows, and it can be opened on a
// clock-only tick when no sensor ran. So everything it reports is cached here too, and republished
// on every wake — otherwise the dashboard shows zeros for the four minutes between measurements.
RTC_NOINIT_ATTR static float    rtc_volt, rtc_crate;              // battery: cell voltage, %/h
// Which way this board reads its cell, remembered from the last measurement tick so the screen can
// answer without bringing the I2C bus up. Garbage out of an uninitialised RTC cannot mislead here:
// the reader tests against the two named values and treats anything else as "not known yet".
RTC_NOINIT_ATTR static uint8_t  rtc_bat_rev;
// The cable as the battery monitor last saw it (0/1; anything else — garbage after a new image — just
// means "read it again"). See follow_cable().
RTC_NOINIT_ATTR static uint8_t  rtc_bat_cable;

// Is the charge finished? ONE rule, in one place.
//
// It used to be written out twice, character for character, in the Telegram reply and on the
// screen. Two copies of one rule are one edit away from telling the owner two different things,
// and this one was about to be edited: v2 answers it from hardware.
static bool charge_finished(bool usb)
{
    if (!usb) return false;
    // v2: the charger itself, through STDBY#. Readable from a GPIO alone, which is what makes it
    // usable here — this runs while drawing, long after the I2C bus went down.
    if (rtc_bat_rev == BatterySource::RevDividerV2) return BatterySource::chargeComplete(true) == 1;
    // v1, and the fallback before the first measurement tick of a new image: inferred, because a
    // gauge is all that board has. Near-full voltage with a rate too slow to be charging is
    // TP4056's termination window. On a v2 board this can call a finished charge "still charging"
    // for one frame, and the next measurement tick corrects it.
    return rtc_volt >= 4.10f && fabsf(rtc_crate) < 0.5f;
}

// The battery as the dashboard shows it — the figure and the bolt — in one place, so the QR screen a
// long press brings up without a network says exactly what the dashboard said a minute ago. The
// charger's verdict outranks the estimate; a bolt means current is flowing (see draw_dashboard()).
static PortalScreen::Battery shown_battery(bool usb, bool charge_done)
{
    PortalScreen::Battery b;
    b.usb = usb;
    // -1 = the cell has not been read; the charger's own verdict outranks the estimate.
    b.percent = charge_done ? 100 : (rtc_soc > 0.0f ? (int)rtc_soc : -1);
    b.charging = usb && !charge_done && rtc_soc > 0.0f;
    return b;
}
RTC_NOINIT_ATTR static float    rtc_bat_ttf;
RTC_NOINIT_ATTR static uint32_t rtc_bat_cycles;
RTC_NOINIT_ATTR static int      rtc_bat_state;
RTC_NOINIT_ATTR static uint8_t  rtc_bat_alerts;
RTC_NOINIT_ATTR static float    rtc_b_temp, rtc_b_hum;            // gas sensor's own climate
RTC_NOINIT_ATTR static float    rtc_gas_ohm, rtc_siaq, rtc_gasp;  // raw gas, static IAQ, gas %
RTC_NOINIT_ATTR static float    rtc_stab, rtc_runin;              // BSEC stabilisation / run-in
// Low-battery lockout. Latched, because the threshold must not be crossed back and forth: a cell
// that has just been rescued from 3.30 V will read higher the moment the load drops, and without
// hysteresis the device would wake, measure, lock, wake again, all the way down.
// Severity of the current air verdict, kept so the cycle can decide whether it is worth waking the
// radio for. The classifier itself runs every measurement regardless — the screen must always tell
// the truth; this is only about whether anyone gets told.
// Outcome of the LAST CO2 attempt, kept across sleeps. A measurement happens every few minutes at
// most and the USB port barely exists during those windows, so the evidence has to survive until
// a tick that can actually be read.
// When the gas calibration was last written to flash, and at what accuracy. RTC alone is not
// enough for it: it survives sleep and restarts but not the power actually going away, and the
// calibration it holds takes HOURS to rebuild from nothing.
RTC_NOINIT_ATTR static time_t   rtc_bsec_nvs_at;
RTC_NOINIT_ATTR static uint8_t  rtc_bsec_nvs_acc;
RTC_NOINIT_ATTR static float    rtc_toff_applied;   // offset last written to the sensor
RTC_NOINIT_ATTR static char     rtc_co2_diag[64];
RTC_NOINIT_ATTR static uint8_t  rtc_alert_tier;
RTC_NOINIT_ATTR static bool     rtc_lockout;
RTC_NOINIT_ATTR static bool     rtc_lock_drawn;
RTC_NOINIT_ATTR static int8_t   rtc_pres_mode;  // barometric forecast, recomputed each measurement
RTC_NOINIT_ATTR static uint8_t  rtc_pres_miss;  // gas samples in a row without a pressure in range
RTC_NOINIT_ATTR static int8_t   rtc_ptrend;
RTC_NOINIT_ATTR static int      rtc_class;                        // air classifier verdict
// The same cascade without moisture. The Air Quality block shows THIS, so that a damp room no
// longer hides the gas word behind «Damp»; rtc_class keeps damp and keeps driving the alerts.
RTC_NOINIT_ATTR static int      rtc_air_class;
RTC_NOINIT_ATTR static char     rtc_cls_txt[40];
RTC_NOINIT_ATTR static AlertState rtc_alert;                      // hysteresis across sleeps
// The debounced class the device is willing to stand behind. It differs from rtc_alert.notified_class
// exactly while a message is owed, which is the whole definition of "something to send".
RTC_NOINIT_ATTR static int      rtc_alert_confirmed;
// The reading that TRIPPED the alert. A message must describe the event it reports, not the moment
// it happened to be delivered — the text said exactly that in a comment and then quoted the live
// value anyway, producing "936 ppm, above the 1100 you set": a sentence that contradicts itself and
// teaches the owner the device does not mean what it says. CO2 beside a person falls fast, so by
// the time a radio window opens the number has always moved on.
RTC_NOINIT_ATTR static uint16_t rtc_alert_co2;
RTC_NOINIT_ATTR static float    rtc_alert_iaq;   // the index that tripped a gas alert, for the same reason
RTC_NOINIT_ATTR static time_t   rtc_alert_at;    // when the reading behind the pending message was taken
// Everything the pending message quotes, frozen with rtc_alert_at at the reading that decided it. The message
// can leave a window or two later, and the numbers it prints must belong to the time it prints.
RTC_NOINIT_ATTR static uint16_t rtc_msg_co2;
RTC_NOINIT_ATTR static float    rtc_msg_iaq, rtc_msg_temp, rtc_msg_hum;
// Frozen with the rest of them: the surface humidity the damp verdict was taken on, and the
// outdoor temperature it was worked out from. A message that quoted today's weather for an alert
// raised this morning would be telling the owner about a wall that no longer exists.
RTC_NOINIT_ATTR static float    rtc_msg_srh, rtc_msg_out;
RTC_NOINIT_ATTR static uint8_t  rtc_msg_acc;
// Did the last attempt to reach the weather service fail? Separate from "is there a cached
// city", which is what wx_state used to be derived from and which stays true for ever.
RTC_NOINIT_ATTR static bool     rtc_wx_failed;
// How many wakes have happened with no valid wall clock. The bootstrap that measures on every
// wake has to be bounded by something, and with no clock it cannot be bounded by time.
RTC_NOINIT_ATTR static uint32_t rtc_blind_wakes;
// Boots that never reached sleep. Incremented before anything can fail and cleared at the one
// moment that proves a boot did its job — arriving at esp_deep_sleep_start(). A device that panics
// early otherwise reboots for ever: this is the only state in which the firmware cannot report its
// own failure, because nothing it writes afterwards ever runs.
//
// In RTC and not NVS on purpose: this is touched twice per wake, about 2900 times a day, and NVS
// has a finite number of writes. The guard is folded with buildStamp() so a new image starts its
// own count — a fresh binary must not inherit the previous one's failures — and so that garbage
// read through a moved RTC layout cannot present itself as "five failures already".
RTC_NOINIT_ATTR static uint32_t rtc_boot_guard;
RTC_NOINIT_ATTR static uint8_t  rtc_boot_incomplete;
RTC_NOINIT_ATTR uint8_t rtc_sec_updated;   // an update was committed; tell the owner once
// RTC, not .bss. The comment above promises "otherwise at the next radio window", and between the
// console window and that next window the device deep sleeps — which wipes .bss. So the fallback
// path this queue was written for could never once have run: a notice not delivered inside the
// window died at the next sleep, silently, and the three retries were unreachable. Reviewed
// 2026-09-18. Cleared in forget_previous_build(), which is also what makes power-on garbage safe.
RTC_NOINIT_ATTR static char    s_sec_note[192];   // the longest of these texts, plus a version string
RTC_NOINIT_ATTR static uint8_t s_sec_tries;
#define BOOT_FAIL_REVERT   5    // enough to be a pattern, few enough to cost seconds
#define BOOT_FAIL_BARE     8    // safe mode itself is not completing: stop raising the radio too

RTC_NOINIT_ATTR static char     rtc_wx_city[24], rtc_wx_desc[26], rtc_wx_icon[6];
RTC_NOINIT_ATTR static float    rtc_wx_temp, rtc_wx_feels, rtc_wx_wind, rtc_wx_lo, rtc_wx_hi;
// Today's low and high, accumulated WIDEN-ONLY. An extreme once seen is kept for the rest of the
// day and never recomputed away: the morning's low is only ever a FUTURE forecast entry for a few
// hours, and recomputing from a fresh forecast later would throw it away and report the remaining
// day's low instead (CLAUDE.md 4.14). Reset happens at local midnight, nowhere else.
RTC_NOINIT_ATTR static float    rtc_day_lo, rtc_day_hi;
RTC_NOINIT_ATTR static int32_t  rtc_day_stamp;      // local day number the pair belongs to
RTC_NOINIT_ATTR static int      rtc_wx_hum;
// The extra cells of the configurable bottom strip. Sentinels rather than zeroes: 0 % cloud and
// 0 % cloud is a real reading, "never fetched" is not, and the strip must be able to
// tell them apart to print dashes.
RTC_NOINIT_ATTR static float    rtc_wx_uv, rtc_wx_dew;
RTC_NOINIT_ATTR static int      rtc_wx_cloud;
// The four-day outlook, one entry per local calendar day. Kept by DATE, not by position: an outlook
// that missed its refresh across a midnight would otherwise show yesterday's "tomorrow" as tomorrow.
// The row matches days to today's date when it is drawn, and a day it has nothing for reads "--".
// Not mirrored to flash like today's min/max: every day of it is still in the future, so the next
// weather window simply fetches it again.
RTC_NOINIT_ATTR static WxDay    rtc_fc[4];

// Days since 1 January 1970 for a calendar date (proleptic Gregorian) — Howard Hinnant's
// days_from_civil. Exact for any date this device will see, no tables and no time zone involved,
// which is the point: a date string from the hub is already a local date.
// --- Reducing the local reading to sea level --------------------------------------------------
//
// The barometric forecast needs the LEVEL as well as the rate (analytics.h), and a level only
// means anything reduced to sea level. The reduction is a multiplier set by altitude — and the
// device is not asked for its altitude, it MEASURES it: MET sends air_pressure_at_sea_level in
// the very response the forecast already costs, so the factor is that number divided by what this
// barometer reads at the same moment. No new request, no setting, nothing for the owner to get
// wrong.
//
// Checked against the geometry on 2026-10-05: this unit read 1005.9 hPa, MET said 1023.2, giving
// 1.0172, and the hypsometric value for its 146 m is 1.0174. Agreement to 0.2 hPa — and that is
// the accuracy the whole gate inherits, which is why the level hysteresis is a full hPa.
//
// In NVS, not RTC: it describes WHERE the device stands, and that outlives a reboot. Re-learned
// on every weather report and blended half and half after the first, so one odd grid value cannot
// drag it.
static constexpr float BARO_K_MIN = 1.00f;   // sea level
static constexpr float BARO_K_MAX = 1.45f;   // ~3000 m, past any room this device will stand in

// The textbook reduction, with the standard atmosphere's 15 C and 6.5 K/km lapse. The temperature
// barely matters here: across 0..25 C the factor for 146 m moves by 0.15 %, which is 0.03 hPa — so
// the air the column is actually at is not worth asking for.
static float baro_k_from_altitude(float metres) {
    const float lapse = 0.0065f * metres;
    return powf(1.0f - lapse / (15.0f + lapse + 273.15f), -5.257f);
}

// Where the factor came from, for the console: an owner told "no forecast" deserves to know
// whether the device never saw weather or never got a city.
enum BaroSrc : uint8_t { BARO_SRC_NONE = 0, BARO_SRC_MEASURED = 1, BARO_SRC_CITY = 2 };

static float baro_factor(uint8_t *src = nullptr) {
    if (src) *src = BARO_SRC_NONE;
    // Measured against a weather report: good to a couple of tenths of a hPa. Always wins.
    float k = 0.0f;
    if (NVSManager::getInstance().getFloat("baro_k", k, 0.0f) && k >= BARO_K_MIN && k <= BARO_K_MAX) {
        if (src) *src = BARO_SRC_MEASURED;
        return k;
    }
    // Fallback: the altitude of the chosen city, carried in by the console with its coordinates.
    // A city-centre average, so a few hPa out in hilly places — but the gate it feeds separates
    // high pressure from low, a step of some 15 hPa, and it is replaced by the measured factor the
    // moment any weather arrives. This is what keeps the plaque alive on a Matter unit, which goes
    // online once during setup and never again.
    float alt = 0.0f;
    if (NVSManager::getInstance().getFloat("wx_alt", alt, NAN) && isfinite(alt) &&
        alt > -500.0f && alt < 9000.0f) {
        const float ka = baro_k_from_altitude(alt);
        if (ka >= BARO_K_MIN && ka <= BARO_K_MAX) {
            if (src) *src = BARO_SRC_CITY;
            return ka;
        }
    }
    return NAN;
}

// Sea-level hPa, or NAN while the device has never seen a weather report — and then there is no
// forecast at all, only the number. Deliberate: a word we cannot stand behind is worse.
static float baro_sea_level(float local_hpa) {
    const float k = baro_factor();
    return isfinite(k) ? local_hpa * k : NAN;
}

static void baro_learn_factor(float p_sea_hpa) {
    if (!(p_sea_hpa > 800.0f && p_sea_hpa < 1100.0f)) return;           // MET sent nothing usable
    if (!(rtc_pres_pa > 30000.0f && rtc_pres_pa < 110000.0f)) return;   // our own barometer is out
    const float fresh = p_sea_hpa / (rtc_pres_pa / 100.0f);
    if (!(fresh >= BARO_K_MIN && fresh <= BARO_K_MAX)) {
        ESP_LOGW(TAG, "BARO: altitude factor %.4f out of range, ignored", fresh);
        return;
    }
    const float had = baro_factor();
    const float k = isfinite(had) ? (had + fresh) / 2.0f : fresh;
    if (isfinite(had) && fabsf(k - had) < 0.0005f) return;              // not worth a flash write
    if (NVSManager::getInstance().setFloat("baro_k", k))
        ESP_LOGI(TAG, "BARO: altitude factor %.4f (this report %.4f) -> sea level %.1f hPa",
                 k, fresh, rtc_pres_pa / 100.0f * k);
}

static int32_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}
// What is CURRENTLY on the glass. Cutting the panel's power in sleep wipes the controller's own
// copy of the previous frame, and a partial update only drives pixels that differ from it — with
// no reference the new digits are laid ON TOP of the old ones. So we re-render the previous frame
// from these values, hand it over as the baseline, and only then draw the new one.
// EVERY field that reaches the glass lives in here. A field read straight from a live variable
// would be re-rendered with its NEW value while replaying the OLD frame, so the baseline would
// not match the glass and the old text would survive as a ghost. The weather did exactly that
// once already (CLAUDE.md 4.14) — hence one struct, no exceptions.
struct Frame {
    char date[16], time[8], co2[8], temp[8], hum[8], pres[12], cal[8], aq[sizeof(rtc_cls_txt)];   // aq: the classifier's word
    bool aqInv;
    bool humDamp;     // the humidity block says «Damp» and inverts (owner, 2026-10-02)
    int8_t aqLevel;   // seven Bosch cells filled up to this class; -1 = none
    bool err;       // climate channel down: SCD41 gives CO2, temperature AND humidity
    // The offset the graph's own time labels were rendered with. The fourth violation of the 4.7
    // rule hid here, in the one field that looks safe because its DATA is in the frame: the
    // timestamps were, but the offset applied to them was read live from NVS, so the first partial
    // draw after a timezone change replayed yesterday's frame with today's offset, the baseline
    // disagreed with the glass, and the old hour digits were never cancelled.
    int16_t tzmin;
    // Header state. In the frame like everything else: the plug appears and disappears with the
    // cable, so read live it would ghost on the next partial update.
    bool night;     // moon in place of the clock: no minute ticks to keep it honest
    bool usb;       // plug icon: on the wire
    bool chg;       // lightning on the battery: actually taking charge
    bool lowbat;    // inverted % pill
    bool wifiFail;  // "!wifi" pill after a sync that did not succeed
    int  batt, outT;
    char city[24], desc[26], icon[8], lo[8], hi[8], ohum[8], wind[8], feels[8];
    // The configurable bottom strip, already formatted. Which three cells are shown is the owner's
    // choice, so `strip` belongs here too: change it in the console and the frame must differ, or
    // the "nothing changed" check would keep the old strip on the glass.
    uint32_t strip;
    char uv[8], sunrise[8], sunset[8];
    // The phase QUANTISED to 48 steps, not the raw fraction: the real phase changes every second,
    // and a frame that never equals the last one would repaint the panel every single minute.
    // Forty-eight steps is one every fifteen hours, which over-samples the ten pictures a 9 px disc
    // can actually show. -1 = no clock, so no phase.
    int8_t moonQ;
    // The outlook row, exactly as drawn: the weekday word, the icon code and both temperatures
    // already in the owner's unit. The weekday is here too, not recomputed while replaying the
    // previous frame — at midnight every name shifts by one, and a replay with the NEW names
    // would leave the old ones on the glass. fc_n 0 = no row.
    int8_t   fc_n;
    char     fc_day[4][8], fc_icon[4][4], fc_hi[4][6], fc_lo[4][6];   // "Sat 12", "10d", "22", "-3"
    // The console's address band. It MUST live in the frame like everything else: it was read from
    // a live argument instead, so once the console closed the previous frame was replayed without
    // the band that was still on the glass — and the band never came off. The graph broke with it,
    // because the band shortens the graph: the replay drew a different-length line, which then
    // could not cancel the one on the glass, and they stacked.
    char     presU[6];         // "hPa" or "mmHg", per the user's setting
    bool     updClock;         // show the last reading's time, prefixed, instead of a live clock
    int8_t   noNet;            // 0 = normal, 2 = offline layout: full-width graph, no outdoor block
    int8_t   presMode;         // 0 number, 1 Rain, 2 Storm (inverted)
    int8_t   ptrend;           // pressure trend arrow: -1 down, 0 flat, +1 up
    char     fresh[8];         // age of the CO2 reading:  "now" / "12m" / "3h" / "2d"
    char     wxFresh[8];       // age of the outdoor weather
    char     setup[40];
    char     setup2[40];   // second row of the band: the console password
    uint16_t g_co2[48];        // graph samples, oldest -> newest
    uint8_t  g_n;
    uint32_t g_t0, g_t1;       // the span the samples actually cover, for the axis labels
};
RTC_NOINIT_ATTR static Frame rtc_prev;
// RTC memory survives a reflash, so after a rebuild that changed the layout above, the "previous
// frame" is the old bytes read through the new struct — nonsense presented as what is on the
// glass, which a partial update would then paint over. A build stamp catches it: when it changes,
// the frame is disowned and the next draw is a full refresh, which needs no reference at all.
// Identifies THIS binary, and it has to identify the binary rather than the moment it was built.
// It was hashed from __DATE__ __TIME__, which made it the single reason the same source could never
// produce the same image twice — and a firmware nobody can reproduce is a firmware nobody can check
// against the repository, which is the first question asked after a bad update and the only one an
// update channel makes urgent.
//
// The ELF hash says the same thing more honestly: identical source gives an identical stamp, so a
// rebuild that changed nothing no longer throws away the RTC state, and any real change still
// invalidates it. IDF computes and embeds this already; nothing is added to the image for it.
static uint32_t buildStamp() {
    const esp_app_desc_t *d = esp_app_get_description();
    uint32_t h = 2166136261u;
    if (d) for (size_t i = 0; i < sizeof(d->app_elf_sha256); i++)
        h = (h ^ (uint32_t)d->app_elf_sha256[i]) * 16777619u;
    return h ? h : 1u;      // zero is the one value RTC garbage is most likely to hold
}
RTC_NOINIT_ATTR static uint32_t rtc_build_stamp;

// The detail word of a boot record. Low byte is the reset reason, bit 8 says the boot never
// reached sleep, bit 9 says this is the first wake of a new image, and the top half carries the
// image's own fingerprint. Packed rather than split across fields because `detail` is the one
// free word in a 32-byte record, and the reader already masks the low byte, so old tools keep
// working against new records.
static inline uint32_t boot_detail(esp_reset_reason_t r, uint32_t bits)
{
    return ((uint32_t)r & 0xFFu) | bits | ((buildStamp() & 0xFFFFu) << 16);
}

// Bumped by the web server on every request — the setup window uses it as "the user is still
// working", so the countdown restarts instead of expiring under their hands.
extern uint32_t global_last_http_ms;
extern int      global_portal_remain;   // seconds left, published to the console UI
extern volatile bool global_screen_dirty;
extern volatile bool global_weather_dirty;
extern volatile bool global_hub_dirty;
extern volatile bool global_portal_close;
extern volatile bool global_factory_req;   // the console asked for a factory reset
extern volatile bool global_reboot_req;    // the console asked for a restart
extern volatile bool global_close_req;     // the console asked to end the session now
extern volatile bool global_poweroff_req;  // ...or for the device to be switched off
extern volatile uint32_t global_temp_seq;  // readings taken with the temperature offset on file
extern volatile int global_calib_req;         // ppm the page wants to calibrate to (0 = idle)
extern volatile int global_calib_result;     // 2 running / 3 asking / 1 done / -1 failed / 0 idle
extern volatile int global_calib_correction;
extern volatile int global_calib_samples;    // run-up readings taken since the request
extern volatile int global_calib_error;      // 1 window closed, 2 sensor refused, 3 no sensor,
                                             // 4 not outdoor air and nobody said to go on
extern volatile int global_calib_lo;         // what the run-up read: lowest, highest, mean (ppm)
extern volatile int global_calib_hi;
extern volatile int global_calib_avg;
extern volatile int global_calib_confirm;
extern volatile int global_calib_unsettled;
extern volatile int global_calib_settled;    // the page's answer while asking: 1 go on, -1 stop
extern volatile int global_wifi_connect_req;    // set by /api/save when credentials are stored
extern volatile int global_wifi_connect_state;
// Telemetry the web console reads through /api/status. It is served straight out of these, so
// nothing appears in the dashboard until the cycle publishes into them.
extern float global_voltage, global_soc, global_soc_raw, global_crate;
extern uint32_t global_btn_noise;
extern uint16_t global_co2;
extern float global_s_temp, global_s_hum, global_b_temp, global_b_hum, global_b_pres, global_b_gas;
extern float global_iaq, global_static_iaq, global_gas_percentage, global_dewpoint;
extern uint8_t global_iaq_accuracy;
extern float global_stab_status, global_run_in_status;
extern int global_pollution_class;
extern std::string global_pollution_text;
extern uint32_t global_boot_count;
extern int   global_bat_state;
extern float global_bat_ttf_h;
extern uint32_t global_battery_cycles;
extern uint8_t  global_bat_alerts;
extern bool  global_bat_low, global_on_usb;
extern float global_out_temp, global_out_uv, global_out_dew;
extern float global_surface_rh;
extern float global_p_sea; extern float global_baro_k; extern uint8_t global_baro_src;
extern int global_out_cloud;
extern std::string global_out_desc, global_out_city, global_out_icon, global_wx_state;

// What the cycle decided on this wake. Published so the Mode tab can show the schedule in force
// rather than the one on file — they differ at night, in Ultra Save, and before the first sync.
int    global_plan_profile   = 1;
bool   global_plan_night     = false;
uint32_t global_plan_meas_min  = 5;
uint32_t global_plan_radio_min = 30;
bool   global_plan_live_clock  = true;
uint32_t global_plan_last_meas = 0;   // unix seconds; 0 = nothing measured yet
uint32_t global_plan_last_co2  = 0;


extern bool     global_portal_keep;   // user tapped the timer pill: stretch the window  // 0 idle, 1 connecting, 2 joined, -1 failed

RTC_NOINIT_ATTR static bool     rtc_prev_valid;
// How many wakes in a row the USB pin has raised without the cable state actually having changed.
// The flight recorder's first run found the reason this has to be counted: with the cable OUT, the
// detect line still produced 45 state changes in eight minutes, the device woke on every edge —
// 207 wakes where 8 were due — and since the plug icon follows the cable, the panel was partially
// refreshed once every four seconds. That is 44x the interval the panel vendor asks for, with 111
// partials in a row and a single cleaning frame.
RTC_NOINIT_ATTR static uint8_t  rtc_usb_spurious;
// How long a wake takes from the timer firing to the pixels being settled. Measured, not assumed,
// and kept separately per kind of tick because they differ by an order of magnitude: a clock-only
// tick is around a second, a measuring one around six. Waking this much EARLY is what puts the new
// minute on the glass at the minute, instead of a second and a half after it.
// Is the panel currently powered? A wake that draws nothing must leave the switch exactly as the
// last one set it, so the state has to outlive the sleep.
RTC_NOINIT_ATTR static bool rtc_panel_powered;
// Consecutive button wakes with nothing pressed. Feeds the back-off in arm_wake_pins().
RTC_NOINIT_ATTR static uint32_t rtc_btn_ghosts;
// Setup windows in a row that a finger-less button wake opened and nobody used. See the classifier.
RTC_NOINIT_ATTR static uint8_t  rtc_portal_unused;
#define PORTAL_UNUSED_MAX 3
// A factory reset that was asked for and not yet carried out. Mirrored in NVS as "fr_pending":
// this copy does not survive the power going away, and the NVS one does.
RTC_NOINIT_ATTR static uint32_t rtc_factory_req;
#define FACTORY_REQ_MAGIC 0xFAC70E5Eu
// Is the logo already the picture on the glass? Redrawing it costs ten seconds of a 4-gray
// refresh and changes nothing a person could see.
RTC_NOINIT_ATTR static bool rtc_logo_shown;
RTC_NOINIT_ATTR static bool rtc_ink_blocked;   // updates paused: the room is outside the panel's range
RTC_NOINIT_ATTR static uint8_t rtc_bat_notified;
// The last thing worth telling the owner, composed ONCE and then handed to whoever can deliver it.
//
// Both channels used to decide for themselves what counted as an event: the device notified from
// a gas calibration of 1 of 3, the hub rule demanded 3 of 3, and the two quietly disagreed about
// the same air. Two opinions is one too many. Now the device decides and writes the finished words
// here; Telegram sends them, the state message carries them, and neither is allowed a view.
RTC_NOINIT_ATTR static char rtc_al_title[56];
RTC_NOINIT_ATTR static char rtc_al_text[640];
RTC_NOINIT_ATTR static char rtc_al_level[10];   // clear | info | advisory | warning | critical
// Deliveries still owed for the text above. Telegram used to be sent whenever that text merely
// EXISTED — and it exists for good, because it lives in RTC memory. So every radio window, and
// every long press that opened the console, re-sent whatever was last said. This counts down
// instead: an event is delivered once, with a couple of retries in later windows if the send
// itself fails, and then never again.
RTC_NOINIT_ATTR static uint8_t rtc_al_tries;
// The hub is also told about each new event on its own, as a message on <topic>/alert (2026-09-22).
// The state message carries the alert too, but repeats it every window for as long as it stands, so
// a hub forwarding it to a phone has to watch one sensor for a CHANGE — and that sensor's name comes
// from whatever the owner called the device. An event needs nothing of the sort: one subscription,
// any hub, any name. rtc_al_seq counts events; rtc_al_hub is the last one the broker acknowledged.
// Unequal means one is owed, and it is retried in the next window until it is taken.
RTC_NOINIT_ATTR static uint32_t rtc_al_seq, rtc_al_hub;
RTC_NOINIT_ATTR static time_t rtc_mqtt_disco_at;   // when Home Assistant was last told what this device is  // low-battery levels already announced: 1=15% 2=10% 4=5%
RTC_NOINIT_ATTR static bool rtc_was_night;     // to catch the moment night ends
RTC_NOINIT_ATTR static uint32_t rtc_lat_clock_ms;
RTC_NOINIT_ATTR static uint32_t rtc_lat_meas_ms;
// Everything between the timer firing and app_main existing — ROM loader, flash init — happens
// before esp_timer exists and cannot be measured from inside. Rather than model it, the residual
// error is fed back: half of it each tick, clamped, until it settles.
RTC_NOINIT_ATTR static int32_t  rtc_sync_trim_ms;
RTC_NOINIT_ATTR static time_t   rtc_last_full;      // when the panel was last fully refreshed
RTC_NOINIT_ATTR static uint32_t rtc_partials;      // partial updates drawn since then

// The LED is the only feedback available once the cable is off. It is held STEADY for as long as
// the device is busy with something the user asked for, and is dark the rest of the time — a
// scheduled wake never lights it. Blink sequences were tried and removed: several flashes a second
// read as a fault indicator, not as an acknowledgement.

// A plain level on a pad. It used to need a no-light-sleep lock: on battery every vTaskDelay let the
// chip doze, the doze isolated this pad, and the light strobed while the device polled the button.
// Light sleep is gone from the firmware (power-management block in bench_screens_run()), so a level
// set here stays set.
static void led(bool on)
{
    gpio_set_level(PIN_LED, on ? 1 : 0);
}

// A press that arrives while the device is busy used to vanish.
//
// The button is only ever read as a reason for waking; between waking and going back to sleep —
// about a second on a clock tick, six on a measurement, twenty in a radio window — nothing looks
// at it. That is three to five per cent of the time, which matches exactly what was reported: it
// works, and occasionally the first press does nothing at all and the second one is fine.
//
// An interrupt costs nothing while idle and catches the press. It cannot be acted on immediately —
// the device is in the middle of a measurement or a network session, and interrupting those would
// trade one lost press for corrupted readings — so it is remembered and honoured at the end of the
// cycle, by waking straight back up and running the tick as if the button had woken it.
static volatile bool s_btn_pressed_while_awake = false;
RTC_NOINIT_ATTR static bool rtc_btn_pending;   // survives the moment of sleep, read on the way back

// What the air did while everyone was asleep.
//
// The radio is off all night in every profile, so a hub sees an eight-hour hole and can say
// nothing about the one stretch a bedroom monitor exists for. Past readings cannot be filled in
// afterwards — Home Assistant timestamps a state when it arrives, not when it was measured, and no
// standard offers a way round that. What CAN be carried across the gap is the shape of the night,
// computed here from the log the device kept anyway and published once in the morning.
RTC_NOINIT_ATTR static uint16_t rtc_ns_co2_max, rtc_ns_co2_avg;
RTC_NOINIT_ATTR static float    rtc_ns_t_min, rtc_ns_t_max, rtc_ns_h_max;
RTC_NOINIT_ATTR static uint32_t rtc_ns_from, rtc_ns_to;   // 0 = nothing to report
// When the night actually began. The summary used to look back a flat twelve hours, so with the
// default window of 23:00-07:00 it reported five hours of the evening as part of the night —
// including the CO2 peak of a room full of awake people, presented to the hub as what happened
// while everyone slept.
RTC_NOINIT_ATTR static uint32_t rtc_night_began;

// Rejected edges survive the sleep, because the question is not "was there noise in this one
// second of awake time" but "is this device being falsely woken over days". It is published with
// the readings for the same reason: during a battery run the serial port does not exist, and a
// counter that can only be read over USB is a counter that cannot be read when it matters.
RTC_NOINIT_ATTR static uint32_t rtc_btn_noise;

// Edges that were not a press. A real press pulls the line to ground and HOLDS it there for tens
// of milliseconds; a coupling spike is over before the handler has finished reading the register.
static volatile uint32_t s_btn_glitches = 0;

// When the finger went down, by the monotonic clock of THIS boot. Only the first edge of a press
// sets it; the bounce that follows must not restart the clock it is meant to measure.
static volatile int64_t s_btn_press_us = 0;
// A press that began BEFORE this boot, in milliseconds already served. See the classification loop.
RTC_NOINIT_ATTR static uint32_t rtc_btn_held_ms;

static void IRAM_ATTR btn_isr(void *)
{
    // This accepted ANY falling edge, and the consequence was not a stray flag: a press caught here
    // makes the device wake back up in 50 ms and treat the next tick as a button press, which turns
    // on the light, forces a full refresh and forces the radio up. So one microsecond of noise on
    // this pin bought a flashing screen at night, a lit LED and a Wi-Fi session — observed on the
    // battery run, in night mode, with nobody near the device.
    //
    // The suspect is the panel's power FET, switched at the end of every cycle a few millimetres
    // away; this handler is armed across exactly that moment. Rather than trust that identification,
    // reject what cannot be a press and count what was rejected, so the noise becomes a number in
    // the log instead of a theory.
    if (gpio_get_level(PIN_USER_BTN) != 0) { s_btn_glitches = s_btn_glitches + 1; rtc_btn_noise++; return; }
    if (!s_btn_pressed_while_awake) s_btn_press_us = esp_timer_get_time();   // first edge only
    s_btn_pressed_while_awake = true;
}

// The same thing as the low branch above, but reachable from a task. PmGate calls it when one of
// the sensor waits ends with the button held down: inside that wait the chip may have been in light
// sleep and the pin's interrupt is off anyway, so the press left no edge for btn_isr to see. Only
// the line itself is left as evidence, and it is read the same way here.
static void note_button_from_nap(void)
{
    if (!s_btn_pressed_while_awake) s_btn_press_us = esp_timer_get_time();
    s_btn_pressed_while_awake = true;
}

// Evidence that the light-sleep gate did something, written where it can still be read a week
// later from a battery run. Self-limiting on purpose: the first 64 measuring wakes of an image,
// about five hours, and then silence — enough to answer "did it work", cheap against a ring of
// 8192 slots, and it re-arms itself on the next image, which is exactly when the question returns.
static void note_light_sleep(void)
{
    // RTC_NOINIT is uninitialised memory, and the counter only ever reaches 64 by its own early
    // return below — so anything past that is the garbage a fresh layout left behind, not a count.
    // This is not a hypothetical: the first version reset it in forget_rtc_state() instead of
    // forget_previous_build(), the garbage read as a huge number, and the feature wrote nothing at
    // all after a flash while looking perfectly healthy.
    if (rtc_ls_notes > 64) rtc_ls_notes = 0;
    if (rtc_ls_notes >= 64) return;
    rtc_ls_notes++;
    uint32_t ms = (uint32_t)(PmGate::slept_us() / 1000);
    uint32_t n  = PmGate::sleeps();
    if (ms > 16383) ms = 16383;
    if (n  > 1023)  n  = 1023;
    EvtLog::getInstance().event(EVT_NOTE, (uint32_t)NOTE_LIGHT_SLEEP | (ms << 8) | (n << 22));
}

// Wait for the finger to come off, but never forever. A press is only finished when the button is
// released, so the code that follows has to wait for it — and an unconditional wait hands the
// device to whatever is holding the button down. A jammed button, or a person who holds it because
// nothing seems to be happening, would starve the watchdog: it panics at a minute, the device
// reboots, wakes on a button that is still down, and does it again — a reset loop that flattens the
// cell instead of showing anything. So it gives up after ten seconds and carries on; a press that
// long was not a press.
// Brings the gas calibration back from flash: the state's clock, the gap since the snapshot, and
// the room's own norm. Two different events need exactly this — a firmware change, which moves
// every RTC variable, and a power loss, which erases them — and they had drifted apart, with the
// power-loss path restoring only the clock and neither the gap nor the norm. One function, so the
// branch a customer's first update takes is the same one every reflash and every dead battery
// exercises here. Must run BEFORE anything initialises BSEC.
static void restore_gas_from_flash(const char *why)
{
    NVSManager::getInstance().init();
    BSECManager::invalidateRtcState();     // flash is the only source now, not a gamble on garbage
    // Arm the gate probe: THIS boot's load is the one worth recording, and nothing after it is.
    BSECManager::getInstance().armLoadProbe();

    uint32_t saved_clk = 0, saved_wall = 0;
    NVSManager::getInstance().getU32("bsec_clk",  saved_clk, 0);
    NVSManager::getInstance().getU32("bsec_wall", saved_wall, 0);
    const time_t now_w = time(NULL);
    int64_t gap = 0;
    if (saved_wall && now_w > 1600000000 && now_w > (time_t)saved_wall) {
        gap = (int64_t)(now_w - (time_t)saved_wall);
        // The ceiling the running clock already uses for a sleep gap. A compressed timeline is
        // survivable; an unbounded one is how a wrong wall clock becomes a gas timeline decades
        // long, and the algorithm answers that by discarding everything.
        if (gap > 3600) gap = 3600;
    }
    // Two seconds on top: the snapshot keeps whole seconds of a clock that had moved on by a fraction,
    // and BSEC takes only increasing timestamps (AN011, bsec_input_t::time_stamp).
    rtc_bsec_ns = ((int64_t)saved_clk + gap + 2) * 1000000000LL;
    {
        // The wall-clock moment this base belongs to; read_sensors() moves the base on from here.
        struct timeval tv; gettimeofday(&tv, NULL);
        rtc_bsec_wall_us = (tv.tv_sec > 1600000000) ? (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec : 0;
    }

    ESP_LOGW(TAG, "%s — gas hand-off: clock %u s +%d s gap", why, (unsigned)saved_clk, (int)gap);
}

// The single fact this whole mechanism rests on: we got all the way to sleep, so this image can
// complete a cycle. Called at every sleep site — there are three, and a boot that reaches any of
// them has done its job.
// Arming the pins that can end a sleep, in one place for all four sleep paths.
//
// The USB choices are deliberately different and not an oversight. The working cycle wants to know
// about ANY change, so it arms for the opposite of the level it is sleeping at. Lockout and safe
// mode only care that a charger has arrived: they are already stopped, and being woken because the
// cable was removed would achieve nothing.
static bool usb_present(void);      // defined below

enum class UsbWake { None, OnPlug, OnChange };

// After this many pin wakes that changed nothing, stop arming the pin until a scheduled wake
// clears the count. Nothing is lost but the instant plug icon during a storm — the timer still
// brings the device up within its normal interval, and it notices the cable then.
static constexpr uint8_t USB_STORM = 8;

// Returns whether the button ended up armed. A caller whose sleep has no timer must look at this:
// if the button is not armed and no timer is set, the device never comes back.
static bool arm_wake_pins(UsbWake usb)
{
    // Espressif's sequence, from "Sleep Modes" in the ESP-IDF docs and the ESP-IDF deep_sleep example
    // (examples/system/deep_sleep/main/gpio_wakeup.c): the pin is configured as an input with
    // gpio_config(), then esp_deep_sleep_enable_gpio_wakeup(). The same docs say wakeup sources are
    // not disabled after a wakeup and have to be disabled with esp_sleep_disable_wakeup_source(), so
    // every sleep starts from none and each caller enables exactly what it wants after this returns.
    //
    // Not doing that is how the logo sleep broke on battery (hardware, 2026-09-11): the power manager
    // had left the timer source enabled (esp_pm_configure with light sleep, pm_impl.c), the logo
    // sleep never set a timer of its own, and it woke almost at once, over and over.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // Levels are read here, after gpio_config() and before any wake is enabled.
    const bool usb_now = (usb != UsbWake::None) && usb_present();   // usb_present() configures the pad
    gpio_config_t btn = {};
    btn.pin_bit_mask = 1ULL << PIN_USER_BTN;
    btn.mode = GPIO_MODE_INPUT;
    gpio_config(&btn);
    const bool btn_down = (gpio_get_level(PIN_USER_BTN) == 0);

    // Arming the level a pin is already at wakes the device the instant it sleeps, for ever. The
    // button has one resting level (R10 holds it high), so a line still low here is left unarmed and
    // the caller's timer brings the device back. The same for a line that keeps waking the device
    // with no press behind it (rtc_btn_ghosts, counted by the wake classifier).
    bool armed = false;
    if (rtc_btn_ghosts >= 8) {
        ESP_LOGW(TAG, "button line produced %u phantom wakes — not arming it this time",
                 (unsigned)rtc_btn_ghosts);
        rtc_btn_ghosts = 0;                 // one clean attempt after the timer brings us back
    } else if (btn_down) {
        ESP_LOGW(TAG, "button still held at sleep entry — not arming it (would wake instantly)");
    } else {
        const esp_err_t be = esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_USER_BTN,
                                                               ESP_GPIO_WAKEUP_GPIO_LOW);
        if (be != ESP_OK) ESP_LOGE(TAG, "button wake arming FAILED: %s", esp_err_to_name(be));
        else              armed = true;
    }

    if (usb == UsbWake::None) return armed;
    // OnPlug waits for the cable to go in; OnChange waits for the level opposite to the current one.
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_USB_DETECT,
                                      (usb == UsbWake::OnChange && usb_now) ? ESP_GPIO_WAKEUP_GPIO_HIGH
                                                                             : ESP_GPIO_WAKEUP_GPIO_LOW);
    if (usb == UsbWake::OnChange)
        ESP_LOGW(TAG, "cable %s — will wake when it is %s",
                 usb_now ? "in" : "out", usb_now ? "pulled" : "plugged in");
    return armed;
}

static void settle_pending_image(void);
static void evt_log_wake(void);   // defined below, where this wake's own numbers are in scope

// Reaching sleep is the whole definition of a boot that worked, so both things that care about it
// hang here: the failed-boot counter resets, and a freshly installed image commits or reverts.
// settle_pending_image() used to sit at one call site out of the four below, which meant an update
// whose first wake took any other path — lockout, safe mode, the logo, the setup portal — stayed
// PENDING_VERIFY and would have reverted later at some unrelated brownout, long after anyone could
// connect the two events.
static inline void boot_reached_sleep(void)
{
    settle_pending_image();
    // The wake summary goes in AFTER settle_pending_image() deliberately: if that decides to
    // revert, the record of the wake that made the decision is already the last thing written, so
    // the log explains the rollback instead of stopping just before it.
    evt_log_wake();
    rtc_boot_incomplete = 0;
}

// What this wake learned about itself. Collected as the cycle runs rather than by a separate test
// afterwards, because the honest question for a new image is not "can it pass a checklist" but
// "did it just do its job" — and the cycle is the job. Nothing here costs anything: they are
// outcomes the code already had and threw away.
static int8_t   s_sensors_ok  = -1;     // of three on the bus, how many answered; -1 = not asked
static int64_t  s_draw_ms     = -1;     // last panel refresh, or -1 if none happened
static bool     s_served_page = false;  // this wake held a console or setup window open

// One record per wake, written from boot_reached_sleep(). Everything in it was gathered as the
// cycle ran, so this costs a single 32-byte page program — well under a millisecond.
static void evt_log_wake(void)
{
    // The wake record's free field carries the rejected-edge counter. It reached 23688 over a
    // 16-day run — about one per wake, which is far too regular to be stray pickup and points at
    // something the device itself does on the way in or out. A cumulative number cannot answer
    // that; per-wake values can, because they say WHICH wakes collect edges.
    EvtLog::getInstance().setDetail(rtc_btn_noise);
    EvtLog::getInstance().wake(0);
}

// Ten seconds, counted from the press itself and not from the end of the three-second signal.
// Far from the long-press threshold on purpose: by then the owner has already been told the press
// was registered, so anything past it is deliberate rather than a slow finger.
static const int BTN_OFF_MS = 10000;

// How long the question stays on the glass before it answers itself with "no". Long enough to read
// four lines and decide, short enough that a press nobody meant does not leave the device asking.
static const int OFF_CONFIRM_S = 30;

static void wait_button_release(const char *what)
{
    int held = 0;
    while (gpio_get_level(PIN_USER_BTN) == 0 && held < 10000) {
        vTaskDelay(pdMS_TO_TICKS(20)); held += 20;
        if ((held % 2000) == 0) esp_task_wdt_reset();
    }
    if (held >= 10000)
        ESP_LOGW(TAG, "button still down after 10 s (%s) — carrying on without it", what);
}

// The last steps of every sleep, in the order they have to happen. The caller has already armed its
// pins with arm_wake_pins(), which clears every wake source first, so the timer is set here, after it;
// a timer of 0 leaves the pins as the only way back. `flush_log` gives the last lines 200 ms to reach
// a serial monitor.
[[noreturn]] static void enter_deep_sleep(uint64_t timer_us, bool flush_log)
{
    if (timer_us) esp_sleep_enable_timer_wakeup(timer_us);
    esp_task_wdt_delete(NULL);          // going to sleep on purpose is not a hang
    if (flush_log) vTaskDelay(pdMS_TO_TICKS(200));
    boot_reached_sleep();               // last: it commits a pending image and stamps the sleep
    esp_deep_sleep_start();
}

// Everything this file keeps in RTC memory about the device's life: readings, weather, schedule,
// alerting, the panel's last frame, the button's history. RTC_NOINIT survives sleep, esp_restart and
// a reflash, so nothing here goes away just because the device stopped being configured.
//
// NOT touched, on purpose: the build stamp and the boot counter, which describe the image rather than
// the device's past; rtc_panel_powered, which describes the panel as it physically is right now and
// has already been acted on this boot; the factory-reset marker, which belongs to its caller; and the
// sleep-latency trims, which are a property of this board, not of anybody's use of it.
// Everything the outdoor weather left behind, back to "never fetched".
//
// It is a function because TWO paths need it and they must not drift: a factory reset, and an image
// change. The second one is why this exists at all. RTC variables move to new addresses on every
// rebuild, so a field not reset there reads whatever the old layout left at that address — and the
// strip's cells are sentinel-guarded, which makes garbage worse than a zero would be: a large
// random int passes the "was it ever fetched" test and the clamp that keeps snprintf inside eight
// bytes then renders it as a plausible 999. That is exactly what shipped in 0.10.16 and what the
// owner saw: three nines in the UV, rain and cloud cells after an over-the-air update.
//
// Resetting on an image change costs today's accumulated high and low, and one radio window with
// dashes instead of weather. Both are cheap against a number nobody can tell is wrong.
static void forget_weather(void)
{
    rtc_wx_failed = false;
    rtc_wx_city[0] = rtc_wx_desc[0] = rtc_wx_icon[0] = '\0';
    rtc_wx_temp = rtc_wx_feels = rtc_wx_wind = 0; rtc_wx_hum = 0;
    rtc_wx_uv = -1.0f; rtc_wx_dew = -999.0f; rtc_wx_cloud = -1;
    // The +-999 sentinels, not zero: 0 C is a temperature a day can genuinely have (see mmok).
    rtc_wx_lo = 999.0f; rtc_wx_hi = -999.0f;
    rtc_day_lo = 999.0f; rtc_day_hi = -999.0f; rtc_day_stamp = 0;
    memset(rtc_fc, 0, sizeof(rtc_fc));   // the outlook was for the old owner's city
}

static void forget_rtc_state(void)
{
    rtc_magic = 0; rtc_tick = 0; rtc_bsec_ns = 0; rtc_bsec_wall_us = 0;
    rtc_last_weather = rtc_last_wx_try = rtc_last_fc = rtc_last_meas = rtc_last_co2 = 0;
    rtc_last_hist = 0; rtc_met_expires = 0;
    rtc_wifi_fail = false; rtc_wifi_fails = 0; rtc_wifi_skip = 0;
    rtc_pres_pa = 0; rtc_pres_miss = 0;
    rtc_co2 = 0; rtc_co2_fails = 0;   // a wiped device is not a broken one
    rtc_temp = rtc_hum = rtc_iaq = rtc_soc = rtc_soc_raw = 0; rtc_acc = 0;
    rtc_volt = rtc_crate = 0;
    rtc_bat_ttf = 0;
    rtc_bat_cycles = 0; rtc_bat_state = 0; rtc_bat_alerts = 0; rtc_bat_notified = 0;
    rtc_b_temp = rtc_b_hum = 0;
    rtc_gas_ohm = rtc_siaq = rtc_gasp = 0; rtc_stab = rtc_runin = 0;
    rtc_bsec_nvs_at = 0; rtc_bsec_nvs_acc = 255;
    rtc_toff_applied = -999.0f; rtc_co2_diag[0] = '\0';
    rtc_lockout = false; rtc_lock_drawn = false;
    rtc_pres_mode = 0; rtc_ptrend = 0;
    rtc_class = 0; rtc_air_class = 0; rtc_cls_txt[0] = '\0';
    rtc_alert = AlertState{}; rtc_alert_tier = 0; rtc_alert_confirmed = 0; rtc_alert_co2 = 0; rtc_alert_iaq = 0; rtc_alert_at = 0;
    rtc_msg_co2 = 0; rtc_msg_iaq = rtc_msg_temp = rtc_msg_hum = 0.0f; rtc_msg_acc = 0;
    rtc_msg_srh = rtc_msg_out = NAN;
    rtc_al_title[0] = rtc_al_text[0] = rtc_al_level[0] = '\0'; rtc_al_tries = 0;
    rtc_al_seq = rtc_al_hub = 0;
    rtc_blind_wakes = 0;
    forget_weather();
    rtc_prev_valid = false; rtc_prev = Frame{};
    rtc_last_full = 0; rtc_partials = 0; rtc_ink_blocked = false; rtc_logo_shown = false;
    rtc_mqtt_disco_at = 0;
    rtc_was_night = false; rtc_night_began = 0;
    rtc_ns_co2_max = rtc_ns_co2_avg = 0; rtc_ns_t_min = rtc_ns_t_max = rtc_ns_h_max = 0;
    rtc_ns_from = rtc_ns_to = 0;
    rtc_usb_spurious = 0; rtc_btn_noise = 0;
    rtc_btn_pending = false; rtc_btn_held_ms = 0; rtc_btn_ghosts = 0;
    rtc_portal_unused = 0;
    BSECManager::invalidateRtcState();   // the gas calibration starts again, in its new room
}

// Everything this device remembers, in every place it keeps it, with one deliberate exception.
// "Erase everything" on this device is a list, not a call: NVS, the measurement ring, the flight
// recorder, the crash dump and RTC memory are separate stores. The reset this replaces wiped NVS and
// left the rest to whatever a later boot decided — which, whenever the history happened to be empty,
// was nothing at all for the flight recorder and the crash dump.
//
// KEPT: the CO2 sensor's forced recalibration. It lives in the sensor's own EEPROM, it corrects that
// physical sensor rather than recording anybody's use of it, and it survives this reset the way it
// survives a battery swap. Nothing here sends the sensor a command. The temperature offset is kept
// too: with "scd_toff" gone, the first measurement takes the sensor's own value into NVS rather than
// writing the default over it (measure()).
//
// Runs at the start of the boot that follows the request, before anything reads the old state. The
// order survives a power cut: the NVS marker that asked for this is the LAST thing erased, so a wipe
// that is interrupted simply runs again on the next boot.
static void factory_wipe(void)
{
    led(true);                                   // somebody asked for this and is waiting for it
    ESP_LOGW(TAG, "FACTORY RESET — erasing everything this device remembers (CO2 and temperature calibration kept)");
    const int64_t t0 = esp_timer_get_time();

    forget_rtc_state();
    // The wall clock itself stays: it is the time, not anybody's data, and a set clock is what lets
    // the next owner's first readings carry dates. Its sync stamp goes, so the next network earns it.
    ClockSync::getInstance().forceClockSync();

    HistStore::getInstance().init();
    HistStore::getInstance().eraseAll();
    EvtLog::getInstance().init();
    EvtLog::getInstance().eraseAll();
    // The crash dump is a snapshot of RAM, and the owner's Wi-Fi name has been read out of one with
    // `strings` before; their password, weather key and bot token live in the same memory.
    {
        const esp_err_t ce = esp_core_dump_image_erase();
        ESP_LOGW(TAG, "crash dump partition: %s", ce == ESP_OK ? "erased" : esp_err_to_name(ce));
    }
    esp_task_wdt_reset();

    // The temperature calibration is carried ACROSS the erase, the way the CO2 one already survives
    // it (owner's decision 2026-09-11 for CO2, 2026-09-27 for temperature). Both are corrections for
    // THIS sensor in THIS enclosure — a property of the device, not of the owner, and the next person
    // to hold it wants a device that reads true, not one that has forgotten how.
    //
    // CO2 survives for free: its correction lives in the SCD41's own EEPROM and the firmware never
    // sends the sensor a factory reset. Temperature's lives in NVS, which is about to go whole, so
    // it is read out here and written back below.
    //
    // The sensor's EEPROM also holds a copy, and scd_sync_offset() adopts it when NVS has nothing —
    // but only if the sensor answers at the first measurement after the wipe. Until it does, the
    // console shows the 4.00 C default, which reads as "never calibrated" rather than "ask again in
    // a minute". Carrying the number keeps it true from the first boot, and the EEPROM copy stays as
    // the second line of defence for a flash erased some other way.
    float keep_toff = 0.0f;
    const bool have_toff = NVSManager::getInstance().getFloat("scd_toff", keep_toff, 4.0f)
                           && keep_toff >= 0.0f && keep_toff <= 20.0f;

    // The cell's gain correction rides across for exactly the same reason, and with a sharper edge:
    // it has no copy anywhere else. Temperature can be recovered from the sensor's own EEPROM and
    // CO2 never leaves it, but this number describes the resistors and the converter on THIS board
    // and lives only here. Losing it to a factory reset would silently move the cutoff — on the v2
    // board measured on 2026-09-29 the device would start guarding a cell at 3.24 V while believing
    // it guarded one at 3.30 — and nothing on the screen would say so.
    float keep_cal = 0.0f;
    const bool have_cal = NVSManager::getInstance().getFloat("bat_cal", keep_cal,
                                                             BatterySource::CAL_NONE)
                          && keep_cal >= BatterySource::CAL_MIN
                          && keep_cal <= BatterySource::CAL_MAX
                          && keep_cal != BatterySource::CAL_NONE;

    // The gain is cached for the boot, and the key it was cached from is about to go. Dropping it
    // here means the write-back below is what the device ends up using — and that a write-back that
    // FAILS leaves the device honestly uncalibrated rather than applying a number it no longer has.
    BatterySource::forgetBoard();

    // NVS last. It holds the request, so erasing it is what marks the wipe as done. Erased whole
    // rather than key by key: Wi-Fi's own namespace, the radio calibration, anything a later version
    // adds, all of it goes without this list having to know about it.
    esp_err_t ne = nvs_flash_erase();
    if (ne == ESP_OK) ne = nvs_flash_init();
    if (ne == ESP_OK) {
        // Written back before anything else can run: a boot that finds nothing here takes the
        // sensor's EEPROM value instead, and the two must not race to decide which one wins.
        if (have_toff) {
            if (NVSManager::getInstance().setFloat("scd_toff", keep_toff))
                ESP_LOGW(TAG, "temperature calibration %.2f C kept across the reset", keep_toff);
            else
                ESP_LOGE(TAG, "temperature calibration %.2f C could NOT be written back — the "
                              "sensor's own copy is the fallback", keep_toff);
        }
        if (have_cal) {
            if (NVSManager::getInstance().setFloat("bat_cal", keep_cal))
                ESP_LOGW(TAG, "cell voltage gain %.4f kept across the reset", keep_cal);
            else
                ESP_LOGE(TAG, "cell voltage gain %.4f could NOT be written back — this board now "
                              "reads its cell uncalibrated and must be measured again", keep_cal);
        }
        rtc_factory_req = 0;
    s_sec_note[0] = '\0'; s_sec_tries = 0;   // RTC now: garbage here would be sent as a notice
    } else {
        ESP_LOGE(TAG, "NVS erase FAILED (%s) — the wipe will run again on the next boot",
                 esp_err_to_name(ne));
    }
    // The first record of the fresh log says why it is fresh.
    EvtLog::getInstance().event(EVT_NOTE, NOTE_FACTORY_WIPE);
    ESP_LOGW(TAG, "factory reset done in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
}

// What this device calls itself in messages. With more than one in the house, "Smart Meteo says the
// air is bad" is useless — the whole point of the notification is knowing which room to walk into.
// Empty (the default) falls back to the product name, so a single device reads exactly as before.
//
// Deliberately NOT wired to the access point's name: the setup QR is a baked-in picture that
// encodes the fixed SSID and password, so renaming the network would leave the QR pointing at a
// network that no longer exists (CLAUDE.md 4.27).
static const char *device_name(char *buf, size_t n)
{
    std::string s;
    NVSManager::getInstance().getString("dev_name", s, "");
    if (s.empty()) return "Smart Meteo";
    snprintf(buf, n, "%s", s.c_str());
    return buf;
}

// When a reading was taken, on the owner's clock (NVS tz_h, the offset the screen uses): "14:05", or
// "13.09 14:05" when it was not today. Empty while the clock has never been set — a message then says
// nothing about time rather than print 1970.
static const char *local_stamp(time_t t, char *buf, size_t n)
{
    buf[0] = '\0';
    const time_t now = time(NULL);
    if (t < 1600000000 || now < 1600000000) return buf;
    float tz = 0; NVSManager::getInstance().getFloat("tz_h", tz, 0.0f);
    const time_t off = (time_t)lroundf(tz * 3600.0f);
    const time_t lt = t + off, ln = now + off;
    struct tm a, b; gmtime_r(&lt, &a); gmtime_r(&ln, &b);
    // Each field is taken modulo 100 so the compiler can see what a reader already knows: these are
    // two-digit fields, and the longest this writes is "dd.mm HH:MM" — eleven characters and a
    // terminator. Without it GCC has to assume an int can print eleven digits, and at -Os, where it
    // inlines enough to look, it refuses the whole file over a truncation that cannot happen.
    const int mday = a.tm_mday % 100, mon = (a.tm_mon + 1) % 100;
    const int hour = a.tm_hour % 100, min = a.tm_min % 100;
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        snprintf(buf, n, "%02d:%02d", hour, min);
    else
        snprintf(buf, n, "%02d.%02d %02d:%02d", mday, mon, hour, min);
    return buf;
}

// Temperature and pressure in the units the owner chose for the screen (NVS u_temp, u_pres).
static const char *fmt_temp(float c, char *buf, size_t n)
{
    uint8_t u = UNIT_TEMP_DEFAULT; NVSManager::getInstance().getU8("u_temp", u, UNIT_TEMP_DEFAULT);
    snprintf(buf, n, "%.1f \xC2\xB0%c", u ? c * 9.0f / 5.0f + 32.0f : c, u ? 'F' : 'C');
    return buf;
}
static const char *fmt_pres(float pa, char *buf, size_t n)
{
    uint8_t u = UNIT_PRES_DEFAULT; NVSManager::getInstance().getU8("u_pres", u, UNIT_PRES_DEFAULT);
    if (pa <= 30000.0f) snprintf(buf, n, "no reading");   // a dead barometer channel, as on the screen
    else if (u)         snprintf(buf, n, "%.0f mmHg", pa * 0.00750062f);
    else                snprintf(buf, n, "%.0f hPa", pa / 100.0f);
    return buf;
}

// Read by the MQTT payload builder, which lives in another file and must not reach into RTC
// variables directly.
const char *alert_title() { return rtc_al_title; }
const char *alert_text()  { return rtc_al_text; }
const char *alert_level() { return rtc_al_level[0] ? rtc_al_level : "clear"; }

// The alert state machine, for the console. Nothing here changes behaviour — it exists because the
// first real end-to-end alert (2026-08-11) arrived LATE and there was no way to see why: the status
// reported the air class and nothing about the machine that decides whether to speak. A class is
// only believed after it holds for two ticks, and a message only leaves once the radio is up, so
// "nothing arrived" has at least four distinct causes that look identical from outside.
int alert_cand_class()    { return rtc_alert.cand_class; }
int alert_cand_count()    { return rtc_alert.cand_count; }
int alert_notified()      { return rtc_alert.notified_class; }
int alert_confirmed()     { return rtc_alert_confirmed; }
int alert_tier()          { return (int)rtc_alert_tier; }
int alert_tries_left()    { return (int)rtc_al_tries; }
long alert_last_notify()  { return (long)rtc_alert.last_notify; }
int alert_co2_latched()   { return rtc_alert.co2_latched ? 1 : 0; }
int alert_damp_latched()  { return rtc_alert.damp_latched ? 1 : 0; }

// One place decides how the panel is left for the sleep, so the three sites cannot drift apart.
// hibernate() puts the controller itself away first either way; what differs is whether its rail
// survives. When the rail is cut, every signal line goes to high impedance rather than being
// grounded — established by bisection in panel_bench.cpp and prescribed by the controller's own
// datasheet. Grounding them left the rail sagging through the region where the controller is alive
// but no longer sane, and it disturbed the pixels on the way down.
static bool s_panel_put_away = false;   // panel_to_sleep() has run this boot

static void panel_to_sleep(EPaper &epd, PanelSleep how = PanelSleep::Powered)
{
    s_panel_put_away = true;
    if (how == PanelSleep::Parked) {
        // Do not wake it just to put it back to sleep. If the rail is already down, the hibernate
        // command would clock SPI into an unpowered panel — feeding it through the ESD diodes we
        // are trying to keep quiet — and bringing the rail up first would spend a second crossing
        // for nothing. Re-latching the gate is the whole job on an hourly wake that drew nothing.
        // Whether the panel is up is a question about NOW, not about how the last sleep left it.
        // init() switches the rail on as a side effect, so any wake that drew has a live panel
        // regardless of the flag — and reading the flag alone cut the supply with no sleep command
        // and no settle at all, which is precisely the thing the vendor calls mandatory. The flag
        // still carries the other case: a wake that drew nothing, on a panel left powered earlier.
        if (epd.isUp() || epd.asleep() || rtc_panel_powered) {
            // A picture drawn this wake already ended with the sleep command (draw()); sending it again
            // would first take the hardware reset that wakes the controller. Otherwise wake it to say it.
            if (!epd.asleep()) {
                epd.init();      // without it there is no SPI to speak the command on
                epd.hibernate();
            }
            // The panel maker's own instruction, and the piece we were missing when cutting the
            // rail greyed the glass overnight: entering sleep is mandatory before power removal,
            // and if that alone does not hold, leave 2-3 seconds between the two. The controller
            // runs its own boosted rails — VGH near +20 V, VGL near -20 V, VCOM — and after the
            // sleep command it needs that long to bring them down in order. Cut sooner and the
            // charge is still on the electrodes when the supply goes, which is the greying.
            //
            // It also explains the asymmetry that started this: pulling the battery does NOT harm
            // the image, because by then the last frame is hours old and those rails are long
            // settled. Our switch was cutting microseconds after the sleep command.
            //
            // Paid once per park, not per wake — a wake that finds the panel already off skips
            // this whole branch.
            vTaskDelay(pdMS_TO_TICKS(PANEL_SETTLE_MS));
            EPaper::holdOffForDeepSleep();

            // And now let the rail finish falling before ANY caller arms a wake pin.
            //
            // Without this the logo screen became an infinite loop: park, arm the button, sleep,
            // and the collapsing rail put an edge on the button line within milliseconds. The
            // device read it as a press, opened the setup screen, timed out after three minutes,
            // drew the logo, parked again — round and round, with the radio up half the time.
            //
            // The coupling is not a guess: 4.65 measured it. Every rejected edge on that line came
            // from a wake that drove the panel, and 450 wakes that left it alone produced exactly
            // zero. Switching the panel's supply is what disturbs the button.
            //
            // Three seconds against a measured two-second decay, and it is paid once per park —
            // a wake that finds the panel already off never enters this branch.
            vTaskDelay(pdMS_TO_TICKS(RAIL_QUIET_MS));
        } else {
            // Already off: re-latch the gate for the coming sleep. Nothing switches, so nothing
            // is disturbed and there is nothing to wait for.
            EPaper::holdOffForDeepSleep();
        }
        rtc_panel_powered = false;
        // The baseline a partial refresh is laid over lives in the panel's own RAM, and that RAM
        // sits on the rail that just went. Whatever is drawn next has nothing to differ from, so
        // it has to be a full refresh — the morning one, or a press at three in the morning.
        rtc_prev_valid = false;
        return;
    }
    epd.hibernate();
    EPaper::holdForDeepSleep();
    rtc_panel_powered = true;
}

// esp_restart() runs every handler registered with esp_register_shutdown_handler() before the reset
// (esp_system.h). A restart the cycle itself makes — an update installed in a radio window — then leaves
// the panel as a sleep does: the vendor's sleep command if the controller is awake, and its supply latched
// on, since a pad hold is kept through a reset (gpio_hold_en, driver/gpio.h). Not when this boot has
// already put the panel away, and not for a panel parked with its supply off and untouched since.
//
// Only on the cycle task, the one that drives the panel. esp_restart() is also called from the httpd task
// (the Restart button, /api/ota) and from the console's install task, and EPaper has no lock: a sleep
// command sent from there could interleave with a frame the cycle task is sending, DC line included. Those
// restarts happen inside a setup window, where every frame already ends with the sleep command (draw(),
// draw_dashboard()).
static EPaper      *s_restart_panel = nullptr;
static TaskHandle_t s_restart_owner = nullptr;
static void panel_before_restart(void)
{
    if (!s_restart_panel || s_panel_put_away) return;
    if (xTaskGetCurrentTaskHandle() != s_restart_owner) return;
    if (!s_restart_panel->isUp() && !s_restart_panel->asleep() && !rtc_panel_powered) return;
    panel_to_sleep(*s_restart_panel, PanelSleep::Powered);
}

// Records an event in the words the owner will actually read. Everything that notifies goes
// through here, so the wording cannot drift between the channels that deliver it.
// Returns false when it refused, which means the previous alert is still owed a delivery. The slot
// is one entry deep, so taking a new event here would silently destroy an undelivered one — and the
// air alert is edge-triggered, so once destroyed it can never be regenerated. A caller whose own
// event is edge-triggered too MUST NOT mark it as handled when this returns false.
static bool set_alert(const char *level, const char *title, const char *text)
{
    if (rtc_al_tries) {
        ESP_LOGW(TAG, "alert [%s] %s held back: '%s' is still owed %u delivery attempt(s)",
                 level, title, rtc_al_title, (unsigned)rtc_al_tries);
        return false;
    }
    snprintf(rtc_al_level, sizeof(rtc_al_level), "%s", level);
    snprintf(rtc_al_title, sizeof(rtc_al_title), "%s", title);
    snprintf(rtc_al_text,  sizeof(rtc_al_text),  "%s", text);
    rtc_al_tries = 3;      // this text is owed a delivery; see rtc_al_tries
    rtc_al_seq++;          // ...and the hub is owed the event; see rtc_al_seq
    ESP_LOGW(TAG, "alert [%s] %s | %s", level, title, text);
    return true;
}

// Hand whatever is owed to Telegram. Called immediately after each set_alert() that can be
// followed by another one in the same window: the air alert and the low-battery warning are
// separate events, and the second used to overwrite the first before either had been sent —
// so a device that went below 15 % during a gas event reported only the battery.
// ---- telling the owner about the device itself ------------------------------------------------
//
// EN 18031-2 [UNM] asks that the user be told about changes affecting the protection of their
// information. Three things qualify and are worth a message: the device password was changed, a
// firmware update went in, and a factory reset was asked for. The person who did it knows already —
// the message is for the case where they did not.
//
// Sign-ins are deliberately NOT sent. Somebody opening the console is standing in front of the
// device with its screen in view, and a message for every session becomes noise, and noise is what
// makes the three that matter get ignored.
//
// The text is queued rather than sent at the moment it happens: these events arrive on the httpd
// task, and Telegram is a TLS handshake and a request on a stack that belongs to a web handler. The
// cycle picks it up — inside the console window it goes out within seconds, otherwise at the next
// radio window.

extern "C" void smart_meteo_security_note(const char *text)
{
    snprintf(s_sec_note, sizeof(s_sec_note), "%s", text);
    s_sec_tries = 3;
}

static void deliver_security_note()
{
    // Terminated defensively: this buffer is RTC memory, which is garbage until
    // forget_rtc_state() clears it, and a missing NUL here would be read past the array.
    s_sec_note[sizeof(s_sec_note) - 1] = '\0';
    if (!s_sec_tries || !s_sec_note[0]) return;
    uint8_t wl_mode = 0, tg_en = 0;
    NVSManager::getInstance().getU8("wl_mode", wl_mode, 0);
    NVSManager::getInstance().getU8("tg_en", tg_en, 0);
    // In Matter mode there is no Telegram and no outbound path of our own; the hub owns messaging.
    if (wl_mode > 1 || !tg_en) { s_sec_tries = 0; s_sec_note[0] = '\0'; return; }
    std::string tok, chat;
    NVSManager::getInstance().getString("tg_tok", tok, "");
    NVSManager::getInstance().getString("tg_chat", chat, "");
    if (tok.empty() || chat.empty()) { s_sec_tries = 0; s_sec_note[0] = '\0'; return; }

    std::string dev;
    NVSManager::getInstance().getString("dev_name", dev, "");
    char text[320];
    snprintf(text, sizeof(text), "\xF0\x9F\x94\x92 %s: %s",
             dev.empty() ? "Smart Meteo" : dev.c_str(), s_sec_note);
    if (Telegram::send(tok, chat, text)) {
        ESP_LOGW(TAG, "security notice sent: %s", s_sec_note);
        s_sec_tries = 0;
        s_sec_note[0] = '\0';
    } else if (--s_sec_tries == 0) {
        ESP_LOGE(TAG, "security notice could not be sent, giving up: %s", s_sec_note);
        s_sec_note[0] = '\0';
    }
}

static void deliver_alert()
{
    if (!rtc_al_tries) return;
    uint8_t wl_mode = 0, tg_en = 0;
    NVSManager::getInstance().getU8("wl_mode", wl_mode, 0);
    NVSManager::getInstance().getU8("tg_en", tg_en, 0);
    if (wl_mode > 1 || !tg_en) { rtc_al_tries = 0; return; }   // hub-only, or Telegram switched off
    std::string tok, chat;
    NVSManager::getInstance().getString("tg_tok", tok, "");
    NVSManager::getInstance().getString("tg_chat", chat, "");
    if (tok.empty() || chat.empty()) {
        rtc_al_tries = 0;
        ESP_LOGW(TAG, "alert to send, but no Telegram token/chat configured");
        return;
    }
    if (Telegram::send(tok, chat, rtc_al_text)) {
        rtc_al_tries = 0;
    } else {
        rtc_al_tries--;
        ESP_LOGE(TAG, "Telegram send FAILED (%u attempt(s) left)", (unsigned)rtc_al_tries);
    }
}

// The owner's alert settings. The save handler stores the CO2 level as an integer: reading it as a float
// fails on the type and hands back the default, which would ignore the level the owner chose.
static AlertConfig load_alert_config()
{
    AlertConfig cfg;
    uint8_t u8 = 1; uint32_t lvl = CO2_ALERT_DEFAULT;
    NVSManager::getInstance().getU32("al_co2", lvl, CO2_ALERT_DEFAULT);
    if (lvl < CO2_ALERT_MIN) lvl = CO2_ALERT_MIN;
    if (lvl > CO2_ALERT_MAX) lvl = CO2_ALERT_MAX;
    cfg.co2_level = (uint16_t)lvl;
    NVSManager::getInstance().getU8("al_co2en", u8, 1); cfg.co2_en = u8;
    NVSManager::getInstance().getU8("al_voc",   u8, 1); cfg.voc_en = u8;
    NVSManager::getInstance().getU8("al_hum",   u8, 1); cfg.hum_en = u8;
    return cfg;
}

// Which low-battery warning is due, if any: 15, 10, 5, or 0 for none.
//
// Returns the LOWEST threshold that has been crossed and not yet announced, so a device that was
// asleep while the charge fell from 18 % to 4 % says "5 %" once rather than working through all
// three. Each level is announced a single time; they re-arm together once the cell is back above
// 20 %, which also covers a swapped battery.
#define BAT_WARN_1 15
#define BAT_WARN_2 10
#define BAT_WARN_3  5
static bool usb_present(void);   // defined further down, with the rest of the power handling
static int battery_alert_level(void)
{
    if (rtc_soc <= 0.0f) return 0;              // no reading yet — nothing to judge
    // Not while it is charging. These messages exist to say "go and plug it in", and the owner has
    // already done that — a device on the cable at 12 % would raise the radio off-schedule to tell
    // its owner to connect the USB power that is currently connected. The thresholds re-arm above
    // 20 % anyway, so a cable that stays in simply means they are never needed.
    if (usb_present()) return 0;
    const int soc = (int)(rtc_soc + 0.5f);
    if (soc <= BAT_WARN_3 && !(rtc_bat_notified & 4)) return BAT_WARN_3;
    if (soc <= BAT_WARN_2 && !(rtc_bat_notified & 2)) return BAT_WARN_2;
    if (soc <= BAT_WARN_1 && !(rtc_bat_notified & 1)) return BAT_WARN_1;
    return 0;
}

// Draw one 4-gray picture and report what actually happened.
// A four-gray picture is not the dashboard, so the same bookkeeping surrounds every one of them:
// the baseline is void afterwards, the elapsed time says whether the transfer really happened, and
// the panel is put to sleep as Good Display ask. Split in two so that the compressed pictures and
// the one composed at run time share it instead of carrying a copy each.
static int64_t draw_begin(const char *name)
{
    // Anything that is NOT the dashboard (logo, QR setup screen) voids the dashboard baseline:
    // the glass no longer holds the frame those saved values describe. The next dashboard must
    // therefore be a FULL refresh — a partial one would compute its differences against a frame
    // that is not there and lay the new readings over the picture that is.
    rtc_prev_valid = false; rtc_prev = Frame{};
    // "LOGO" exactly. The test was the first two letters, and "LOW BATTERY" begins with them too.
    rtc_logo_shown = (strcmp(name, "LOGO") == 0);
    ESP_LOGW(TAG, "drawing %s ...", name);
    return esp_timer_get_time();
}

static void draw_finish(EPaper &epd, const char *name, int64_t t0)
{
    const long long ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGW(TAG, "%s drawn in %lld ms | BUSY=%d | free heap %u",
             name, ms, gpio_get_level(EPD_BUSY_PIN), (unsigned)esp_get_free_heap_size());
    // The white frame, a sleep command and the vendor's two seconds come first (display4Gray), so a
    // healthy draw takes well over this and one whose transfer went nowhere does not.
    if (ms < 4000) ESP_LOGE(TAG, "TOO FAST — the refresh did not really run");
    if (!epd.sessionOk()) ESP_LOGE(TAG, "%s: SPI error or BUSY timeout — the picture may be incomplete", name);
    // Good Display's tips in GDEY042T81_Arduino.ino ask that the panel enter sleep mode when the refresh
    // is completed, and their demo sends EPD_DeepSleep() after every frame. The setup window that follows
    // a picture can keep the device awake for a quarter of an hour.
    epd.hibernate();
}

// The setup screen, composed into planes at run time.
static void draw(EPaper &epd, const char *name, const uint8_t *p24, const uint8_t *p26)
{
    const int64_t t0 = draw_begin(name);
    epd.display4Gray(p24, p26);
    draw_finish(epd, name, t0);
}

// The three pictures that ship in the image: logo, LOW BATTERY and the bare setup screen. They are
// run-length coded and expanded a plane at a time inside the driver — no buffer to find here.
static void draw_rle(EPaper &epd, const char *name,
                     const uint8_t *r24, size_t n24, const uint8_t *r26, size_t n26)
{
    const int64_t t0 = draw_begin(name);
    epd.display4GrayRle(r24, n24, r26, n26);
    draw_finish(epd, name, t0);
}

// The Wi-Fi setup screen for this device: the picture with its own access-point password and the QR
// code that carries it. Composed in two heap planes rather than static ones — thirty kilobytes of
// .bss would come out of the heap in every mode, for a screen most wakes never draw.
//
// bat, when given, goes in the top-right corner the way the dashboard draws it (owner, 2026-10-29:
// whoever switches the device on should see where the charge stands). nullptr leaves the corner as
// the picture has it — safe mode, which must not touch the bus, and a cell nobody could read.
static void draw_setup_screen(EPaper &epd, const char *name, const PortalScreen::Battery *bat)
{
    uint8_t *p24 = (uint8_t *)heap_caps_malloc(PortalScreen::kPlaneBytes, MALLOC_CAP_8BIT);
    uint8_t *p26 = (uint8_t *)heap_caps_malloc(PortalScreen::kPlaneBytes, MALLOC_CAP_8BIT);
    if (p24 && p26 && PortalScreen::compose(p24, p26, BENCH_AP_SSID, WiFiManager::apPassword())) {
        if (bat) PortalScreen::drawBattery(p24, p26, *bat);
        draw(epd, name, p24, p26);
    } else {
        // Still the instructions and the network name; without the password nobody can join, so
        // this is logged as the failure it is.
        ESP_LOGE(TAG, "%s: setup screen not composed (%s) — drawing it without QR and password", name,
                 (p24 && p26) ? "QR or glyph refused" : "no memory");
        draw_rle(epd, name, portal_art_plane24_rle, PORTAL_ART_PLANE24_RLE_LEN,
                 portal_art_plane26_rle, PORTAL_ART_PLANE26_RLE_LEN);
    }
    heap_caps_free(p24);
    heap_caps_free(p26);
}

// Park the sensors in their lowest state. Until the device has been set up there is nothing to
// measure for, and "we never talk to them" is NOT the same as "they are off": they sit on the
// 3.3 V rail permanently (there is no power switch — a deliberate design choice), and the CO2
// sensor idles far above its sleep figure until it is explicitly told to power down. Same for
// the fuel gauge, whose low-power mode has to be asked for. The gas sensor already boots asleep.
static void park_sensors(void)
{
    I2CManager::getInstance().init();
    i2c_master_bus_handle_t bus = I2CManager::getInstance().getBusHandle();
    if (!bus) { ESP_LOGW(TAG, "no I2C bus — cannot park the sensors"); return; }

    SCD41 scd;
    // power_down takes the sensor from IDLE to sleep (SCD4x datasheet v1.7 §3.11.3). wakeToIdle()
    // gets it there first from whatever it was left in: asleep, idle, or measuring periodically.
    if (scd.init(bus)) {
        if (scd.wakeToIdle() && scd.powerDown()) ESP_LOGW(TAG, "CO2 sensor parked (power down)");
        else                                     ESP_LOGW(TAG, "CO2 sensor did not reach idle / power down");
    } else {
        ESP_LOGW(TAG, "CO2 sensor did not answer — cannot park it");
    }

    BatterySource::park(bus);
}

// The settings the Mode tab writes, read back on every wake. The tab promises specific intervals;
// this is what makes the device keep that promise.
//
// Scheduling is by WALL CLOCK, not by counting wakes. A counter drifts here because wakes are not
// all scheduled: the button and the USB cable both wake the device, and every one of those would
// otherwise shift the cadence the user was shown.
struct Plan {
    uint8_t  profile   = 1;      // 0 Performance / 1 Optimal / 2 Ultra Save / 3 Custom
    bool     night     = false;
    bool     no_net    = false;  // the owner set this device up without a network at all
    uint32_t meas_min  = 5;      // CO2 + gas interval in force right now
    uint32_t radio_min = 30;     // network interval; 0 = radio off entirely
    bool     live_clock = true;  // repaint every minute, or only when something was measured
    bool     offline    = false; // radio off by configuration: there will never be weather
    bool     matter     = false; // Matter over Thread: no Wi-Fi at all outside a console session
};

// How long ago, short enough for the tag next to a reading. The screen promises freshness with
// these, so they have to be real: a permanent "now" beside a two-hour-old number is a lie the user
// cannot detect. Below a minute and a half it reads "now" — the tag updates once a minute at best,
// so finer than that would be false precision.
static void fmtAge(time_t age, char *out, size_t n)
{
    if (age < 0)        snprintf(out, n, "now");
    else if (age < 90)  snprintf(out, n, "now");
    else if (age < 5400) snprintf(out, n, "%dm", (int)((age + 30) / 60));
    else if (age < 172800) snprintf(out, n, "%dh", (int)((age + 1800) / 3600));
    else                snprintf(out, n, "%dd", (int)(age / 86400));
}

// Night can wrap past midnight (23 -> 7), so it is not a simple range test.
static bool inNight(int hour, int from, int to) {
    if (from == to) return false;
    return (from < to) ? (hour >= from && hour < to) : (hour >= from || hour < to);
}


// One snapshot of everything the gas algorithm needs to wake up knowing what it knew. The four keys
// are only meaningful TOGETHER: the state blob without its clock hands the next boot a timeline
// running backwards and it drops the run-in. Written from two places, the hourly backup and the
// moment before an update takes over, and it exists as a function precisely so those two cannot
// drift apart.
//
// Refuses when BSEC did not run this wake. A snapshot taken where the algorithm never initialised
// is not a snapshot of anything, and writing one over a good backup is how 4.32 destroyed a real
// calibration.
static bool save_calibration_snapshot(uint8_t acc, const char *why)
{
    if (!BSECManager::getInstance().isRunning()) {
        ESP_LOGW(TAG, "calibration snapshot skipped (%s): the gas algorithm did not run this wake",
                 why);
        return false;
    }
    if (!BSECManager::getInstance().saveStateNVS()) return false;
    const time_t wall_now = time(NULL);
    NVSManager &n = NVSManager::getInstance();
    n.setU8("bsec_acc", acc);                       // what the backup is worth
    // Wall time of the snapshot. The gas clock is virtual and only advances while we run, so
    // restoring it later would hand the algorithm a timeline with the gap missing. Knowing WHEN it
    // was taken lets the gap be added back on the other side.
    n.setU32("bsec_wall", (uint32_t)(wall_now > 1600000000 ? wall_now : 0));
    n.setU32("bsec_clk",  (uint32_t)(rtc_bsec_ns / 1000000000LL));
    rtc_bsec_nvs_at  = wall_now > 1600000000 ? wall_now : 1600000001;
    rtc_bsec_nvs_acc = acc;
    ESP_LOGW(TAG, "gas calibration backed up to flash (acc %u/3) — %s", acc, why);
    return true;
}

// Called by ota_install.cpp when an update has been written and the device is about to restart
// into it, from either path — the scheduled window or the console button. A hook because the state
// that has to survive an update lives here, not in the installer; and it is the same call the
// scheduled path makes, so the two endings cannot drift apart.
extern "C" void smart_meteo_ota_handoff(void)
{
    save_calibration_snapshot(rtc_acc, "handing over to a new image");
}

// What the console's "Running now" card reads. It promises to report the device rather than the
// saved settings, so it has to be refreshed whenever the device's answer could have changed —
// which inside an open setup window is every time somebody saves something.
static void publish_plan(const Plan &p)
{
    global_plan_profile    = p.profile;
    global_plan_night      = p.night;
    global_plan_meas_min   = p.meas_min;
    global_plan_radio_min  = p.radio_min;
    global_plan_live_clock = p.live_clock;
}

static Plan readPlan(time_t now)
{
    Plan p;
    auto &nvs = NVSManager::getInstance();
    uint8_t u8 = 1;
    nvs.getU8("profile", u8, 1); p.profile = (u8 > 3) ? 1 : u8;

    // Presets, overridden by whatever the user actually saved. Custom has no preset of its own and
    // falls back to Optimal until its own values are stored.
    static const uint32_t SCD_DEF[3]  = { 5, 15, 30 };
    static const uint32_t WIFI_DEF[3] = { 15, 30, 0 };
    const uint8_t pidx = (p.profile <= 2) ? p.profile : 1;
    uint32_t s_int = SCD_DEF[pidx], w_int = WIFI_DEF[pidx];
    nvs.getU32("s_int", s_int, s_int);
    nvs.getU32("w_int", w_int, w_int);
    uint32_t s_night = s_int * 2;             // at night, half as often unless told otherwise
    nvs.getU32("s_int_n", s_night, s_night);

    uint8_t nm_en = 1, nm_sh = 23, nm_eh = 7;
    nvs.getU8("nm_en", nm_en, 1);
    nvs.getU8("nm_sh", nm_sh, 23);
    nvs.getU8("nm_eh", nm_eh, 7);
    // Night mode used to be forced on in Ultra Save, on the belief that it was "essential to reach
    // the long battery life". Measured 2026-09-21 with the rebuilt model, it is worth 5 % there and
    // nothing else: Ultra already has no live clock and no scheduled radio, so the only thing night
    // still does is double the CO2 interval — 30 minutes to 60. That is eight fewer single shots a
    // day, almost entirely the SCD41's own charge. Five per cent is not worth an overnight reading
    // that can be an hour old, and it is certainly not worth deciding for the owner. The other
    // profiles keep the same switch they always had; this only stops one of them overriding it.

    float tz = 0; nvs.getFloat("tz_h", tz, 0.0f);
    if (nm_en && now > 1600000000) {
        int h = (int)(((now + (time_t)(tz * 3600)) / 3600) % 24);
        if (h < 0) h += 24;
        p.night = inNight(h, nm_sh, nm_eh);
    }

    // A device the owner set up without Wi-Fi never raises its radio: there is nothing to fetch,
    // nowhere to publish and nobody to notify. It also happens to be the longest-lived
    // configuration this device has, since the radio is its largest single expense.
    //
    // This has to happen BEFORE radio_min is derived, and it did not. The interval was zeroed
    // afterwards, so p.offline came out right while p.radio_min kept the profile's default — and
    // radio_min is what the schedule actually reads. An offline device therefore went on planning
    // sessions, and since the network name left over from the failed join is still in NVS, it went
    // on trying to join it. The one configuration advertised as the longest-lived was the shortest.
    uint8_t no_wifi = 0; nvs.getU8("no_wifi", no_wifi, 0);
    // Matter mode keeps its radio for Thread: there is no scheduled Wi-Fi window at all, so for the
    // schedule, the screen and the alerting it is a device without a network (owner, 2026-09-16:
    // weather is lost in this mode). Its console sessions are raised by the button, not by the plan.
    { uint8_t wl = 0; nvs.getU8("wl_mode", wl, 0); p.matter = (wl == 2); }
    if (p.matter) no_wifi = 1;
    if (no_wifi) w_int = 0;
    // Kept apart from `offline` below, which is also true for Ultra Save. For the screen
    // the two are the same — no outdoor weather either way — but for alerting they are
    // opposites: Ultra Save has a network and simply does not use it on a schedule, so an
    // event may still raise the radio; this one has nowhere to send anything, ever.
    p.no_net = (no_wifi != 0);
    // A hub that is told once and never again is worse than no hub: Home Assistant shows the
    // reading from the moment of setup for ever, with no indication it has stopped. Ultra Save and
    // a Custom interval of zero both do exactly that, because discovery goes out on the first
    // session and nothing schedules a second. So in hub mode the radio keeps a floor — an hour is
    // still eight sessions a day less than Optimal, and it is the difference between a live sensor
    // and a dead one.
    { uint8_t wl = 0; nvs.getU8("wl_mode", wl, 0);
      if (wl == 1 && !p.no_net && w_int == 0) w_int = 60; }

    p.meas_min  = p.night ? s_night : s_int;
    if (p.meas_min < 1) p.meas_min = 1;
    p.radio_min = p.night ? 0 : w_int;        // the radio sleeps through the night in every profile
    // Zero stays zero (radio off); anything else gets a floor, because storage may already hold a
    // value written before the interval was clamped at the API, and this one turns straight into
    // how often the device transmits.
    if (p.radio_min != 0 && p.radio_min < 5) p.radio_min = 5;
    // Night silences the radio too, but only until morning — that is not the same thing. This is
    // about the profile: with the interval set to zero the device will never go online at all, so
    // there is no outdoor weather to show and the screen should stop reserving room for it.
    p.offline   = (w_int == 0);

    uint8_t live = 1; nvs.getU8("live_clk", live, 1);
    if (p.profile == 2) live = 0;             // Ultra Save shows the time of the last reading
    p.live_clock = live && !p.night;          // at night the moon replaces the clock (CLAUDE.md 4.14)
    return p;
}

// Whether the device is in its night window. Published so the hub can tell a sleeping device from
// a broken one — the radio is off all night in every profile, and a rule watching for silence
// would otherwise report a fault every single night.
bool device_is_night() { return readPlan(time(NULL)).night; }

// Has the owner finished setting this device up?
//
// Not the same question as "is it on a network", and conflating the two is what made a device with
// no Wi-Fi useless. CO2, temperature, humidity, the log and the whole screen need no network at
// all; only the clock, the outdoor weather and the hub do. Somebody with a 5 GHz-only router, a
// corporate network, or no Wi-Fi whatsoever bought a CO2 monitor and is entitled to a CO2 monitor.
//
// So there are two ways to be set up: a network that has actually been joined, or a deliberate
// choice to run without one. Both are the owner saying "I am done"; neither is a guess.
bool device_configured()
{
    std::string ssid;
    uint8_t ok = 0, offline = 0, prov = 0;
    NVSManager::getInstance().getString("ssid", ssid, "");
    NVSManager::getInstance().getU8("wifi_ok", ok, 0);
    NVSManager::getInstance().getU8("no_wifi", offline, 0);
    NVSManager::getInstance().getU8("provisioned", prov, 0);
    // A device that has been set up before and now has a network name it has not joined yet — the
    // owner typed a new one, or a typo — is still set up. It runs its cycle, shows that the network
    // is unreachable and lets a long press open its own access point to fix it. Treating it as new
    // sent a working installation back to the logo, asleep until somebody pressed the button.
    const bool joined = !ssid.empty() && (ok || prov);
    // Choosing Matter in the console is the third way of being done: the device then pairs itself
    // with a smart home and needs no Wi-Fi to work.
    return joined || offline || MatterMode::selected();
}

// Reads the night back out of the history ring. Called once, when night ends.
static void summarise_night(time_t now, time_t started)
{
    struct Acc { uint32_t n, co2sum, co2max; float tmin, tmax, hmax; } a =
        { 0, 0, 0, 999.0f, -999.0f, 0.0f };
    HistStore::getInstance().init();
    HistStore::getInstance().query((uint32_t)started, (uint32_t)now, 600,
        [](const HistRec &r, void *ctx) {
            Acc *x = (Acc *)ctx;
            if (r.co2 == 0) return;                       // a record with no CO2 reading
            x->n++; x->co2sum += r.co2;
            if (r.co2 > x->co2max) x->co2max = r.co2;
            const float t = r.temp_d / 10.0f;
            if (t < x->tmin) x->tmin = t;
            if (t > x->tmax) x->tmax = t;
            if (r.hum > x->hmax) x->hmax = r.hum;
        }, &a);

    if (a.n < 3) { rtc_ns_from = rtc_ns_to = 0; ESP_LOGW(TAG, "night summary: too few records"); return; }
    rtc_ns_co2_max = (uint16_t)a.co2max;
    rtc_ns_co2_avg = (uint16_t)(a.co2sum / a.n);
    rtc_ns_t_min = a.tmin; rtc_ns_t_max = a.tmax; rtc_ns_h_max = a.hmax;
    rtc_ns_from = (uint32_t)started; rtc_ns_to = (uint32_t)now;
    ESP_LOGW(TAG, "night summary from %u readings: CO2 peak %u, average %u, %.1f-%.1f C, up to %.0f %%",
             (unsigned)a.n, (unsigned)rtc_ns_co2_max, (unsigned)rtc_ns_co2_avg,
             rtc_ns_t_min, rtc_ns_t_max, rtc_ns_h_max);
}

// Handed to the payload builder in another file.
uint16_t night_co2_max() { return rtc_ns_co2_max; }
uint16_t night_co2_avg() { return rtc_ns_co2_avg; }
float    night_t_min()   { return rtc_ns_t_min; }
float    night_t_max()   { return rtc_ns_t_max; }
float    night_h_max()   { return rtc_ns_h_max; }
bool     night_valid()   { return rtc_ns_from != 0 && rtc_ns_to != 0; }


// Is the cell too low to keep working? Read the gauge and nothing else — this runs before the
// sensors, the radio or the screen, because the whole point is to stop spending what is left.
//
// The threshold is the regulator's, not a preference: below it the 3.3 V rail can no longer be
// held and the device would brown out mid-write. Coming back out needs a clear margin above it,
// or the reading would flap as the load comes and goes.
static bool battery_too_low(bool may_read)
{
    // On a minute tick the cached voltage is enough: a cell that was above the threshold sixty
    // seconds ago has not crossed it since, and bringing the I2C bus up to confirm costs more
    // than the answer is worth. The gauge is read on measurement ticks, which is also when its
    // reading is wanted for everything else.
    // One threshold for both paths, clamped where it is read: a stored value can predate the clamp in
    // /api/save, and the two paths used to disagree about it.
    // Built in rather than stored (battery_spec.h): this is the only software guard the cell has,
    // and on the v2 board, with the fuel gauge gone, it will be the only guard at all.
    const float lowv = BATTERY_CUTOFF_V;
    if (!may_read) {
        // The cable is checked here too, and it has to be: the rule below is "on the cable the
        // charger is in charge", but it lived only in the path that reads the gauge. So a device
        // already locked out, then plugged in, kept answering "locked" on every minute tick — and
        // sleep_low_battery() arms "wake when the cable goes in" on a cable that is ALREADY in,
        // which fires the instant it sleeps. The device then booted and slept in a tight loop until
        // the next gas measurement came due and the reading path released it: up to five minutes of
        // continuous wakes, on a nearly flat cell, drawing nothing and explaining nothing. Reading
        // one pin costs a gpio_config(); it is the cheapest thing in this function.
        if (rtc_lockout && usb_present()) {
            rtc_lockout = false; rtc_lock_drawn = false;
            ESP_LOGW(TAG, "charger connected — releasing the low-battery lockout");
            return false;
        }
        if (rtc_volt > 2.0f && !rtc_lockout && rtc_volt < lowv) rtc_lockout = true;
        return rtc_lockout;
    }

    I2CManager::getInstance().init();
    i2c_master_bus_handle_t bus = I2CManager::getInstance().getBusHandle();
    // Whether a charger is attached does not depend on the gauge answering, and it is the only way
    // out of lockout. Both failure paths below used to return before that check, so a device that
    // had correctly locked out and then lost its gauge stayed locked out FOR EVER: every hourly
    // wake took the early return and slept again — no screen, no radio, no console, and the button
    // could not help either, because this runs before all of them. Plugging in the charger did
    // nothing. The only way out was pulling the cell to force a power loss. It needs a second,
    // independent fault to reach, which is exactly when a device must not become unrecoverable.
    auto charger_escape = []() {
        if (!usb_present()) return false;
        rtc_lockout = false; rtc_lock_drawn = false;
        ESP_LOGW(TAG, "battery unreadable but the charger is connected — releasing lockout");
        return true;
    };
    if (!bus) {                                   // cannot tell: keep whatever we decided before
        if (charger_escape()) return false;
        return rtc_lockout;
    }

    // Whichever way this board reads its cell — gauge on v1, divider on v2. The guard itself is
    // unchanged by the revision: it compares volts to a built-in threshold and needs no percent.
    float v = 0;
    if (!BatterySource::readVoltage(bus, v) || v < 2.0f) {
        if (charger_escape()) return false;
        return rtc_lockout;
    }
    rtc_volt = v;

    // On the cable the charger is in charge; never lock out while it can recover on its own.
    if (usb_present()) { rtc_lockout = false; rtc_lock_drawn = false; return false; }

    if (!rtc_lockout && v < lowv)          { rtc_lockout = true;  ESP_LOGE(TAG, "battery %.3f V below %.2f V — locking out", v, lowv);
        // Both directions, because the pair is the evidence: a lockout with no matching release is
        // a device that stopped, and the voltage at each edge says whether the threshold or the
        // gauge was at fault.
        EvtLog::getInstance().event(EVT_POWER, (uint32_t)lroundf(v * 1000.0f) | 0x10000u); }
    else if (rtc_lockout && v > lowv + 0.15f) { rtc_lockout = false; rtc_lock_drawn = false; ESP_LOGW(TAG, "battery recovered to %.3f V — resuming", v);
        EvtLog::getInstance().event(EVT_POWER, (uint32_t)lroundf(v * 1000.0f)); }
    return rtc_lockout;
}

// ---- the working cycle, in the order the user asked for --------------------------------------
// wake -> sensors (I2C brought up and parked again afterwards) -> internet (time, then weather)
// -> ONE screen draw with everything -> sleep. Drawing last is the point: the screen should never
// show half the story while the rest is still arriving.

// Is the cable in? Read the pin rather than the fuel gauge's charge rate: after every boot the
// gauge reports nonsense for a while (a charge of +17 %/h with nothing plugged in has been seen),
// and each tick here IS a fresh boot. The pin is honest on this board revision (CLAUDE.md 4).
// Handed to the installer so it does not open I2C a second time in the same wake just to find out
// whether it may spend the charge. Both are readings this cycle has already taken.
extern "C" float smart_meteo_soc(void)  { return rtc_soc; }
extern "C" bool  smart_meteo_on_usb(void) { return usb_present(); }

static bool usb_present(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << PIN_USB_DETECT);
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
    return gpio_get_level(PIN_USB_DETECT) == 0;
}

// The cable state that is allowed to CHANGE THE PICTURE, as opposed to the raw read above which
// the wake-arming path needs untouched (4.32: reading through gpio_config takes the pad back out
// of RTC mode, so nothing may be inserted between that read and the arming call).
//
// Sampled WITHIN one wake rather than across several, on purpose. A real cable event wakes the
// device by itself, so the icon must still follow it at once (4.14) — requiring agreement across
// wakes would delay a genuine plug by minutes on a timer schedule. And nothing is sampled at all
// unless the raw read disagrees with what is already on the glass, so the ordinary wake pays
// nothing for this.
static bool usb_for_frame(bool shown)
{
    if (usb_present() == shown) return shown;
    int votes = 0;
    for (int i = 0; i < 5; i++) {
        if (usb_present()) votes++;
        if (i < 4) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (votes >= 4) return true;      // clearly connected
    if (votes <= 1) return false;     // clearly not
    return shown;                     // ambiguous: leave the picture alone rather than flicker
}

// Copy the cached readings into the globals /api/status serves. Called on every wake, not only
// after a measurement: the console can be opened at any moment, including on one of the four
// clock-only ticks between measurements, and it should then show the last real reading rather
// than a screenful of zeros.
static void publish_globals(void)
{
    global_btn_noise = rtc_btn_noise;
    global_co2    = rtc_co2;
    global_s_temp = rtc_temp;    global_s_hum = rtc_hum;
    global_b_temp = rtc_b_temp;  global_b_hum = rtc_b_hum;
    global_b_pres = rtc_pres_pa; global_b_gas = rtc_gas_ohm;
    global_iaq    = rtc_iaq;     global_static_iaq = rtc_siaq;
    global_gas_percentage = rtc_gasp;
    global_iaq_accuracy   = rtc_acc;
    global_stab_status    = rtc_stab;
    global_run_in_status  = rtc_runin;
    global_dewpoint = (rtc_hum > 0.0f)
                    ? Analytics::getInstance().calculateDewPoint(rtc_temp, rtc_hum) : 0.0f;
    global_pollution_class = rtc_class;
    global_pollution_text  = rtc_cls_txt;

    global_voltage = rtc_volt;  global_soc = rtc_soc;  global_crate = rtc_crate;
    // RTC_NOINIT: a value outside a percentage is bytes from an older layout, not a reading.
    global_soc_raw = (rtc_soc_raw >= 0.0f && rtc_soc_raw <= 100.0f) ? rtc_soc_raw : rtc_soc;
    global_bat_ttf_h      = rtc_bat_ttf;
    global_battery_cycles = rtc_bat_cycles;
    global_bat_alerts     = rtc_bat_alerts;
    global_on_usb   = usb_present();
    // The monitor's own verdict when it has one; the cable is only a fallback for the very first
    // cycles, before the slope has anything to work with.
    global_bat_state = rtc_bat_state ? rtc_bat_state : (global_on_usb ? 1 : -1);
    global_bat_low   = (rtc_soc > 0.0f && rtc_soc < 15.0f);
    global_boot_count = rtc_tick;

    global_out_temp = rtc_wx_temp;
    global_out_desc = rtc_wx_desc;
    global_out_city = rtc_wx_city;
    global_out_icon = rtc_wx_icon;
    global_out_uv = rtc_wx_uv; global_out_dew = rtc_wx_dew;
    global_out_cloud = rtc_wx_cloud;
    global_wx_state = rtc_wx_failed ? "err" : (rtc_wx_city[0] ? "ok" : "off");
}

// The sensor's temperature offset brought in line with NVS "scd_toff" — only if it actually differs
// from what was last applied, so a settled device never writes to the sensor's EEPROM at all. Settings
// are read and written in the idle state only (§3.7), so the caller has just woken the sensor or read it.
static void scd_sync_offset(SCD41 &scd)
{
    float want = 4.0f;
    const bool stored = NVSManager::getInstance().getFloat("scd_toff", want, 4.0f);
    if (!stored) {
        // Nothing stored here — a new device, a factory reset or an erased flash. The sensor keeps
        // its offset in its own EEPROM (§3.7.1), and that may be the owner's calibration from before
        // the erase: take it over instead of writing the default across it (owner's decision,
        // 2026-09-17; on 2026-09-17 an erased flash replaced a calibrated 0.95 C with 4.00 C).
        // A value outside the datasheet's 0-20 C is not a calibration, and the default goes in.
        float have = 0.0f;
        if (!scd.getTemperatureOffset(have)) {
            ESP_LOGW(TAG, "SCD41 offset not readable — retrying at the next measurement");
        } else if (have >= 0.0f && have <= 20.0f) {
            if (NVSManager::getInstance().setFloat("scd_toff", have)) {
                ESP_LOGW(TAG, "SCD41 offset %.2f C kept from the sensor (none stored)", have);
                rtc_toff_applied = have;
            }
        } else {
            NVSManager::getInstance().setFloat("scd_toff", 4.0f);   // applied just below, next time
            ESP_LOGW(TAG, "SCD41 offset %.2f C out of range — the default 4.00 C is stored", have);
        }
    }
    // Written as !(x <= 0.05) so that garbage in RTC memory — a NaN compares false both ways —
    // counts as "not applied" instead of silently disabling the write for good.
    else if (!(fabsf(want - rtc_toff_applied) <= 0.05f)) {
        float have = 0.0f;
        if (!scd.getTemperatureOffset(have)) {
            // Not read: nothing is marked as applied, so the next measurement tries again. Up to
            // 0.9.43 a failed read was taken for "already correct", and the offset never reached
            // the sensor until the stored value changed.
            ESP_LOGW(TAG, "SCD41 offset not readable — retrying at the next measurement");
        } else if (fabsf(have - want) > 0.05f) {
            ESP_LOGW(TAG, "SCD41 offset %.2f C -> %.2f C (persisting)", have, want);
            if (scd.setTemperatureOffset(want) && scd.persistSettings()) rtc_toff_applied = want;
        } else {
            rtc_toff_applied = want;      // already correct in the sensor; nothing to write
        }
    }
}

// Whether a reading taken now reflects the offset on file: the one applied before the shot is the one
// stored. A reading taken just before a calibration was carried into the sensor does not.
static bool scd_offset_settled(void)
{
    float want = 4.0f;
    return NVSManager::getInstance().getFloat("scd_toff", want, 4.0f) &&
           fabsf(want - rtc_toff_applied) <= 0.05f;
}

// Read CO2/temperature/humidity and the gas sensor. Returns true if anything was measured.
// The outdoor temperature the damp judgement may use, or NAN when there is none it should.
//
// A wall's temperature follows the weather, so this is what makes the verdict seasonal (see
// Analytics::surfaceRH). Six hours is deliberately generous: masonry lags the air by hours anyway,
// so a reading that age still describes the wall better than no reading at all — and the window
// matches the one the hub-weather rule already uses. Beyond it, or with no clock to measure age
// with, the device says so by returning NAN and the classifier falls back to the room number.
static float outdoor_for_damp()
{
    const time_t now = time(NULL);
    if (now < 1600000000 || rtc_last_weather <= 0) return NAN;     // no trustworthy clock
    if (now - rtc_last_weather > 6 * 3600) return NAN;             // too old to describe the wall
    if (!std::isfinite(rtc_wx_temp)) return NAN;
    // 0.0 is what the fields hold before the first fetch, and -273 C would be a parse accident.
    if (rtc_wx_temp < -90.0f || rtc_wx_temp > 70.0f) return NAN;
    return rtc_wx_temp;
}

// One battery reading through the full monitor: the ceiling on the cable, the display layer, the
// cycle count, the alert flags. Part of every measurement, and on its own when only the cable moved
// (follow_cable()). count_for_selftest is false on that second path: the new image's self-test asks
// how many sensors answered a MEASUREMENT, and a wake that measured nothing must leave it at "not
// asked" (-1) — counting the gauge there would turn it into "asked, one answered" and fail the image.
static bool read_battery(i2c_master_bus_handle_t bus, float &soc, bool count_for_selftest)
{
    bool ok = false;
    MAX17048 gauge;
    if (!gauge.init(bus)) return false;
    {
        if (count_for_selftest) {
            s_sensors_ok++;
            EvtLog::getInstance().setSensor(ES_GAUGE, true);
        }
        static bool bat_begun = false;
        if (!bat_begun) { BatteryMonitor::getInstance().begin(bus); bat_begun = true; }
        // 0 is "not measured yet" (the reset blocks write it); compensating the cell model for 0 °C on
        // that sentinel skews the charge estimate until the first real reading arrives.
        //
        // Only where there IS a gauge. gauge.init() above asks the chip nothing, so on a v2 board it
        // "succeeds" and this read of CONFIG failed on every measuring wake with an I2C error about a
        // part the board does not have (seen in the log on 2026-09-30).
        if (BatterySource::revision(bus) == BatterySource::RevGaugeV1
            && rtc_temp != 0.0f && rtc_temp > -40.0f && rtc_temp < 85.0f) gauge.setTempCompensation(rtc_temp);

        BatteryInfo bi;
        // Wall-clock seconds, not BSEC's clock. What reads it — the charging ceiling's hours on the
        // cable, the divider's half-hour average — measures differences over hours, and rtc_bsec_ns
        // only moves on a GAS tick, and even then AFTER this call. time(NULL) survives deep sleep
        // on the RTC timer and advances whether or not anything was measured; a clock step is
        // handled at the other end (battery_monitor.cpp). The discharge rate is not here at all:
        // it comes from the history ring (power_estimate.h).
        const bool usb = usb_present();
        BatteryMonitor::getInstance().update(bus, (double)time(NULL), bi, usb);
        rtc_bat_cable = usb ? 1 : 0;
        // Remember how this board reads its cell, for the screen and for Telegram: both run with
        // the bus already down and cannot probe for themselves.
        rtc_bat_rev = (uint8_t)BatterySource::revision(bus);
        if (bi.valid) {
            soc            = bi.soc;
            rtc_soc_raw    = bi.soc_raw;
            rtc_volt       = bi.voltage;
            rtc_crate      = bi.crate;
            rtc_bat_state  = bi.charge_state;
            rtc_bat_ttf    = bi.ttf_h;
            rtc_bat_cycles = bi.cycles;
            rtc_bat_alerts = bi.alert_flags;
            ok = true;
            ESP_LOGW(TAG, "battery: %.1f %% | %.3f V | %+.2f %%/h | cycles %u%s",
                     bi.soc, bi.voltage, bi.crate, (unsigned)bi.cycles,
                     bi.ic_reset ? " | gauge was reset" : "");
        }
    }
    return ok;
}

// The cable came or went since the battery was last read: read it now, not at the next measuring tick.
// What the glass shows is decided by the display layer (battery_display.h) from what IT sees of the
// cable — "100 % for four hours after a finished charge" needs it to see the finished charge — and it
// only looked on measuring ticks, every five minutes. Found by the owner on 2026-10-29: a full device
// plugged in, the charger said done and the glass said 100 (the frame reads the charger itself);
// unplugged inside those five minutes, and the glass said 96 — the display layer had never seen the
// cable at all. One reading of the divider or the gauge, milliseconds, and only on a wake where the
// cable state differs from the last one the battery saw.
static void follow_cable(void)
{
    const bool usb = usb_present();
    if (rtc_bat_cable == (usb ? 1 : 0)) return;
    I2CManager::getInstance().init();
    i2c_master_bus_handle_t bus = I2CManager::getInstance().getBusHandle();
    if (!bus) return;
    float soc = rtc_soc;
    if (!read_battery(bus, soc, false)) return;
    if (soc > 100.0f) soc = 100.0f;
    if (soc < 0.0f)   soc = 0.0f;
    rtc_soc = soc;
    ESP_LOGW(TAG, "cable %s — battery read now, not at the next measuring tick", usb ? "in" : "out");
}

static bool read_sensors(float &co2, float &t, float &h, float &pres, float &iaq, uint8_t &acc,
                         float &soc, bool with_co2, bool with_gas, bool *co2_ok = nullptr,
                         bool *bme_ok_out = nullptr, bool *bat_ok_out = nullptr)
{
    I2CManager::getInstance().init();
    i2c_master_bus_handle_t bus = I2CManager::getInstance().getBusHandle();
    if (!bus) { ESP_LOGE(TAG, "I2C bus unavailable"); return false; }

    // The CO2 sensor needs ~5 s for a single shot, so start it FIRST and let the gas sensor's
    // heater run inside that window instead of after it.
    SCD41 scd;
    bool scd_ok = false;
    bool scd_init = false, scd_shot = false, scd_idle = false;
    uint16_t raw_co2 = 0; int waited_ms = -1; int64_t shot_us = 0;
    if (with_co2) scd_init = scd.init(bus);
    s_sensors_ok = 0;
    if (scd_init) s_sensors_ok++;
    EvtLog::getInstance().setSensor(ES_SCD, scd_init);
    if (with_co2 && scd_init) {
        // Sensirion's power-cycled single shot (SCD4x datasheet v1.7 §3.11): wake_up,
        // measure_single_shot, wait its 5000 ms, read_measurement, power_down. wakeToIdle() confirms
        // the sensor is idle first, and brings it back from periodic mode if something left it there.
        //
        // NB: the temperature offset is NOT touched here. It used to be, and it cost a day: the
        // check sat between waking the sensor and triggering the measurement, and writing it
        // persists to the sensor's EEPROM — 800 ms, in the middle of a conversion that then returned
        // nothing at all. It is applied after the reading instead, and only when it changes.
        scd_idle = scd.wakeToIdle();
        if (scd_idle) {
            // Held in RAM and lost at every power_down, so sent before every shot (§3.7.5).
            if (rtc_pres_pa > 0.0f) scd.setAmbientPressure(rtc_pres_pa);
            scd_shot = scd.measureSingleShot();
            if (scd_shot) shot_us = esp_timer_get_time();
        }
    }

    // The battery, now that the CO2 sensor is busy. It used to be read before the trigger,
    // which simply postponed the trigger — and the conversion behind it is the longest thing
    // in the wake, so anything ahead of it lengthens the whole tick. Read here it is free:
    // the sensor is converting either way.
    // The full battery monitor rather than a bare register read. It owns the things a single
    // reading cannot give: the quickstart when the gauge itself has lost power, temperature
    // compensation of the cell model, the discharge slope measured over hours (the gauge's own
    // rate figure cannot resolve a discharge this slow), the cycle count, and the alert flags the
    // gauge latched while the processor was asleep.
    if (read_battery(bus, soc, true) && bat_ok_out) *bat_ok_out = true;

    // Gas sensor through BSEC. BSEC dictates the heater profile and the cadence; feeding it
    // anything else is what used to leave accuracy stuck at 0 forever (see CLAUDE.md 4.6).
    //
    // And only when it is due. A button press is not: the comment on the press path already says
    // the gas sensor is deliberately left alone, because its rhythm is fixed and cannot be hurried
    // (4.18). The press ran it anyway, which cost three things — the handshake spun out its whole
    // twelve-second budget waiting for a call time that was minutes away, the schedule was then
    // stamped so the NEXT real sample was skipped, and the verdict was recomputed with a gas
    // deviation of zero, so the glass could read "Good" in the middle of a gas event.
    BME690 bme;
    BSECOutputs out = {};
    bool bme_ok = false;
    bool pres_fresh = false;   // a pressure inside the BME690's operating range arrived this call
    const bool bme_up = with_gas && bme.init(bus);
    if (bme_up && BSECManager::getInstance().init()) {
        s_sensors_ok++;
        EvtLog::getInstance().setSensor(ES_BME, true);
        // The saved state is loaded inside init(), between configuration and subscription (Bosch's order).
        const int64_t t0 = esp_timer_get_time();
        // BSEC's timestamps follow real time: nanoseconds, increasing (AN011 5.2.2.4), and a gas call
        // within 6.25 % of the next_call it asked for (AN011 3.2, 3.3.4.1). The device sleeps between
        // calls, so the base lives in RTC and moves on by the wall-clock time since the end of the last
        // loop, read just before this one. It used to move on after the reading, by the interval that had
        // just ended, which told BSEC the previous gap instead of the real one.
        {
            struct timeval tv; gettimeofday(&tv, NULL);
            const int64_t wall_us = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
            int64_t step_us = wall_us - rtc_bsec_wall_us;
            const bool wall_ok = tv.tv_sec > 1600000000 && rtc_bsec_wall_us > 1600000000LL * 1000000LL;
            // No usable wall clock, or one stepped backwards by a sync: one gas period, the schedule's own
            // interval. More than an hour is held at an hour, the ceiling the flash restore uses too.
            if (!wall_ok || step_us < 0)            step_us = (int64_t)GAS_SEC * 1000000LL;
            else if (step_us > 3600LL * 1000000LL)  step_us = 3600LL * 1000000LL;
            rtc_bsec_ns     += step_us * 1000LL;
            rtc_bsec_wall_us = wall_us;
        }
        int64_t next_ns = 0;
        while (!bme_ok && (esp_timer_get_time() - t0) < 12000000LL) {
            const int64_t now_ns = rtc_bsec_ns + (esp_timer_get_time() - t0) * 1000LL;
            esp_task_wdt_reset();          // BSEC's handshake waits out its own heater profile
            if (now_ns < next_ns) {
                // Wait it out in ONE block, not in fifty-millisecond slices. BSEC states exactly
                // when it wants to be called again, so there is nothing here to poll for, and one
                // block lets the power manager lower the clock for the whole wait. (Written for
                // automatic light sleep, which made this wait cost 180 uA. Light sleep was removed
                // on 2026-09-11; the wait is now an idle core.)
                int64_t wait_ms = (next_ns - now_ns) / 1000000LL;
                const int64_t left_ms = 12000 - (esp_timer_get_time() - t0) / 1000;
                if (wait_ms > left_ms) wait_ms = left_ms;
                if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms + portTICK_PERIOD_MS - 1) + 1);   // at least
                else             vTaskDelay(1);
                continue;
            }
            BSECSensorRequest req = {};
            BSECManager::getInstance().getSensorControl(now_ns, req);
            next_ns = req.next_call_ns;
            if (req.trigger) {
                float rt = 0, rh = 0, rp = 0, rg = 0;
                bool gas_valid = false;
                if (bme.readDataWithHeater(req.heater_temp, req.heater_dur, req.os_temp,
                                           req.os_pres, req.os_hum, req.run_gas, rt, rh, rp, rg,
                                           gas_valid)) {
                    if (rp > 30000.0f && rp < 110000.0f) { pres = rp; rtc_pres_pa = rp; pres_fresh = true; }
                    // Bosch's example processes a sample only when the sensor marks the gas reading
                    // valid (bsec_integration.c, BME69X_GASM_VALID_MSK); anything else is dropped.
                    if (gas_valid)
                        bme_ok = BSECManager::getInstance().processData(now_ns, req, rt, rh, rp, rg,
                                                                         gas_valid, out);
                    else
                        ESP_LOGW(TAG, "gas reading not valid — sample not given to BSEC");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        // The base moves on with the loop, so the next wake's step starts after the last timestamp given.
        {
            const int64_t spent_us = esp_timer_get_time() - t0;
            rtc_bsec_ns      += spent_us * 1000LL;
            rtc_bsec_wall_us += spent_us;
        }
        if (bme_ok) {
            iaq = out.iaq; acc = out.accuracy;
            rtc_siaq  = out.static_iaq;   rtc_gasp  = out.gas_percentage;
            rtc_gas_ohm = out.raw_gas;    rtc_stab  = out.stabilization_status;
            rtc_runin = out.run_in_status;
            rtc_b_temp = out.temperature; rtc_b_hum = out.humidity;
            BSECManager::getInstance().saveState();       // RTC: every cycle, costs nothing

            // Flash: on a change of accuracy, or once an hour. Not every cycle — that would be
            // 288 blob writes a day into a memory with a finite number of them. But often enough
            // that pulling the power costs at most an hour of learning rather than all of it.
            //
            // The clock goes with it. BSEC measures its own time, and a state restored next to a
            // clock that has restarted at zero looks like time running backwards: the algorithm
            // drops its run-in and starts over, which is exactly what the saved state was meant to
            // prevent. The pair is only meaningful together.
            const time_t wall_now = time(NULL);
            const bool acc_changed = (acc != rtc_bsec_nvs_acc);
            const bool hour_passed = (wall_now > 1600000000 && rtc_bsec_nvs_at > 0 &&
                                      wall_now - rtc_bsec_nvs_at >= 3600);
            // Never trade a good backup for a worse one. The stored copy is the only thing that
            // survives a power loss — or a firmware update, which moves every RTC variable and so
            // throws the in-memory copy away. Until now the timestamp that governs this lived in
            // that same RTC memory: after an update it read zero, the code concluded there was no
            // backup, and immediately wrote the freshly-restarted empty state over hours of
            // calibration. The protection destroyed the thing it protected.
            //
            // So a state that knows less than the stored one is only written when the stored one
            // is genuinely stale — a day old — which covers a real recalibration without letting a
            // reboot erase the work.
            uint8_t nvs_acc = 255;
            NVSManager::getInstance().getU8("bsec_acc", nvs_acc, 255);
            const bool worse = (nvs_acc != 255) && (acc < nvs_acc);
            const bool stale_day = (wall_now > 1600000000 && rtc_bsec_nvs_at > 0 &&
                                    wall_now - rtc_bsec_nvs_at >= 86400);
            if (worse && !stale_day) {
                ESP_LOGW(TAG, "gas calibration %u/3 is below the saved %u/3 — keeping the backup",
                         acc, nvs_acc);
            } else if (acc_changed || hour_passed || rtc_bsec_nvs_at == 0) {
                save_calibration_snapshot(acc, "hourly backup");
            }
        }
        // The load source is carried out to here on purpose. It is decided in the first two
        // seconds after reset, and on a device that sleeps between wakes the USB port has not
        // finished enumerating by then, so the line that reports it at the point of decision
        // cannot be read at all -- two button windows produced only tick tails. Whether hours of
        // calibration came back is worth one word in a line that IS readable.
        ESP_LOGW(TAG, "gas sensor: IAQ %.0f acc %u/3 | pressure %.0f Pa | state from %s",
                 out.iaq, out.accuracy, pres, BSECManager::getInstance().loadSource());
    } else if (bme_up && !BSECManager::builtIn()) {
        // A build without Bosch's BSEC (bsec_manager_none.cpp). The sensor is still read for what
        // it measures by itself — pressure, so the barometer and the rain forecast keep working —
        // with the heater off, because heating the plate is only worth it to an algorithm that turns
        // the resistance into an index, and that algorithm is the part this build does not have.
        // Oversampling as the IAQ configuration asks for (2x / 16x / 1x). bme_ok stays false: it
        // means «BSEC gave outputs», and everything downstream that keys on it is about the index.
        s_sensors_ok++;
        EvtLog::getInstance().setSensor(ES_BME, true);
        float rt = 0, rh = 0, rp = 0, rg = 0;
        bool gas_valid = false;
        if (bme.readDataWithHeater(0, 0, BME69X_OS_2X, BME69X_OS_16X, BME69X_OS_1X, false,
                                   rt, rh, rp, rg, gas_valid)) {
            if (rp > 30000.0f && rp < 110000.0f) { pres = rp; rtc_pres_pa = rp; pres_fresh = true; }
            rtc_b_temp = rt; rtc_b_hum = rh;
        }
        ESP_LOGW(TAG, "gas sensor: no BSEC in this build | pressure %.0f Pa", pres);
    }
    // A barometer that stops answering must not leave its last number standing — on the glass, in the
    // log, over MQTT and in the CO2 sensor's pressure compensation. After three gas samples in a row
    // without a pressure inside the BME690's operating range (datasheet: 300-1100 hPa) the reading is
    // dropped, and the device shows what a barometer that never worked shows: none.
    if (with_gas) {
        if (pres_fresh) {
            rtc_pres_miss = 0;
        } else if (rtc_pres_miss < 255 && ++rtc_pres_miss >= 3 && rtc_pres_pa > 0.0f) {
            ESP_LOGW(TAG, "no pressure in %u gas samples — dropping the last reading (%.0f Pa)",
                     (unsigned)rtc_pres_miss, rtc_pres_pa);
            rtc_pres_pa = 0.0f; pres = 0.0f;
        }
    }

    // Collect the CO2 reading the heater window was overlapping with.
    //
    // The ready flag is NOT trusted until the measurement has had its time. A single shot takes
    // about five seconds, and the sensor will happily report "ready" long before that — because
    // the flag belongs to the PREVIOUS conversion. Reading it then returns that stale sample:
    // temperature and humidity look perfectly normal while CO2 comes back as zero, which is
    // exactly what was on the screen. Measured on hardware: ready at 450 ms, CO2 = 0.
    const int64_t SHOT_MS = SCD41::SINGLE_SHOT_MS;   // Table 9: read after 5000 ms
    // No trigger, nothing to wait for. Without this the loop below would block for the full
    // conversion time on every one of its two hundred passes — a quarter of an hour awake —
    // because `since` stays at zero when there is no start time to measure from. Polling used to
    // hide that; blocking does not, which is the price of blocking and worth paying once.
    if (with_co2 && shot_us == 0)
        ESP_LOGW(TAG, "CO2: no single shot was started — nothing to wait for");
    for (int i = 0; with_co2 && shot_us != 0 && i < 200 && !scd_ok; i++) {
        const int64_t since = (esp_timer_get_time() - shot_us) / 1000;
        // The conversion takes a known five seconds, so wait for it rather than asking ninety-four
        // times whether it is done yet. Most of this wait is already overlapped with the gas
        // sensor's heater above; whatever is left is blocked in one piece for the same reason.
        if (since < SHOT_MS) {
            esp_task_wdt_reset();
            // Rounded UP to whole ticks, plus one: pdMS_TO_TICKS rounds down, and a remainder under
            // one tick became vTaskDelay(0), which returns at once — the loop then spent its two
            // hundred passes in microseconds and reported a reading that was never read.
            {
                // The five seconds this blocks for are 46 % of everything this device spends awake
                // (flight recorder, 2026-09-19). Light sleep is permitted for exactly this wait and
                // nothing else; the button can still wake the chip out of it.
                PmGate::Nap nap((uint32_t)(SHOT_MS - since));
                vTaskDelay(pdMS_TO_TICKS(SHOT_MS - since + portTICK_PERIOD_MS - 1) + 1);
            }
            continue;
        }
        {
            bool rdy = false;
            if (scd.isDataReady(rdy) && rdy) {
                uint16_t c = 0; float st = 0, sh = 0;
                // A zero here is not a reading either — the sensor cannot measure no CO2 at all.
                if (scd.readMeasurement(c, st, sh) && c > 0) {
                    co2 = c; t = st; h = sh; scd_ok = true;
                    raw_co2 = c; waited_ms = (int)since;
                }
            }
        }
        if (!scd_ok) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(50)); }
    }
    // No reading. Sensirion's recovery is reinit — reload the settings from EEPROM — and, if that is
    // not enough, a power cycle (datasheet §3.10.5), which the power_down below and the wake_up at
    // the next measurement already are. A factory reset is not a recovery: it erases the fresh-air
    // calibration (§3.10.4). Up to 0.9.43 the firmware did exactly that, automatically, and one
    // failed read was enough: readMeasurement() leaves its outputs at zero on a bus or CRC error,
    // and those zeros passed the "CO2 = 0 with plausible T/H" test.
    if (with_co2 && scd_idle && !scd_ok) {
        ESP_LOGE(TAG, "CO2 sensor gave no reading (%s) — reinit",
                 shot_us ? "after the single shot" : "no single shot started");
        const bool ri = scd.reinit();
        EvtLog::getInstance().event(EVT_SENSOR, SENS_SCD_REINIT | (ri ? 1u : 0u));
    }
    // Now that the reading is in, the offset may be attended to — and only if it actually differs
    // from what was last applied, so a settled device never writes to the sensor's EEPROM at all.
    // Settings are read and written in the idle state only (§3.7).
    if (with_co2 && scd_idle) {
        // A reading taken with the offset now on file is one a temperature calibration may be
        // measured against (postTempCalHandler); one taken before a new offset reached the sensor
        // is not, or the same gap would be subtracted twice.
        if (scd_ok && scd_offset_settled()) global_temp_seq = global_temp_seq + 1;
        scd_sync_offset(scd);
    }

    if (with_co2 && scd_init) scd.powerDown();   // power_down, 0x36e0 (§3.11.3)

    // The verdict, from the user's own alert settings.
    const AlertConfig cfg = load_alert_config();
    // Two sensors, two rhythms, and the verdict must respect both — which is the honest thing for
    // the owner. The gas algorithm owns its five minutes and a button press falls off that beat, so
    // this used to be gated on a gas sample outright: press at 1500 ppm, get silence. But stuffiness
    // is CO2 and damp is humidity, and both come from the OTHER sensor, read fresh on this very tick.
    //
    // So: whatever gas figure is current — this tick's if there was one, otherwise the last real
    // sample — goes in beside the fresh CO2. Nothing is invented, nothing is cleared without
    // evidence, and a gas verdict simply stands until its own sensor speaks again.
    int cls = rtc_class; std::string txt = rtc_cls_txt;
    if (bme_ok || scd_ok) {
        // iaq/acc already hold the cached values on a tick without a gas sample, and so do t/h on a
        // tick that did not read the SCD41 — scd_ok is what tells the damp counter which it is.
        int air = rtc_air_class;
        const float t_out = outdoor_for_damp();
        Analytics::getInstance().classifyAir((uint16_t)co2, iaq, t, h, scd_ok, acc, t_out, cfg,
                                             rtc_alert, cls, txt, air);
        // Kept for /api/status: «Damp» with no way to see what it was judged on is the same dead
        // end the old fixed threshold was, only seasonal.
        global_surface_rh = Analytics::surfaceRH(t, h, t_out);
        // Without BSEC the gas classes never come (they need accuracy >= 1), so class 0 would say
        // «Warming up» for ever — a promise this build cannot keep. Stuffy and Damp still apply.
        if (!BSECManager::builtIn() && cls == 0) txt = "No air-quality index";
        rtc_class = cls; rtc_air_class = air;
        snprintf(rtc_cls_txt, sizeof(rtc_cls_txt), "%s", txt.c_str());
    }

    // Is there anything new to say? Decided here, on the measurement tick, where it costs nothing.
    // It used to be decided inside the radio window, which sounds equivalent and is not: the flag
    // that RAISED that window was the current state rather than a pending event, so while the air
    // stayed bad the device went online on every single wake — every five minutes, and every
    // minute with the live clock — to ask a question it had already answered.
    // Only on a tick that measured something. A tick with no input is not evidence of anything: the
    // decision simply waits for the next real measurement, which is never more than five minutes away.
    if (bme_ok || scd_ok) {
        int confirmed = rtc_alert_confirmed;
        const bool pending = Analytics::getInstance().alertPending(cls, cfg, rtc_alert, confirmed);
        rtc_alert_confirmed = confirmed;
        // Freeze the number that justified it, before the air moves on.
        if (pending) {
            rtc_alert_at = time(NULL);
            rtc_msg_co2 = (uint16_t)co2; rtc_msg_iaq = iaq; rtc_msg_temp = t; rtc_msg_hum = h; rtc_msg_acc = acc;
            rtc_msg_srh = global_surface_rh; rtc_msg_out = outdoor_for_damp();
        }
        if (pending && confirmed != 0) { rtc_alert_co2 = (uint16_t)co2; rtc_alert_iaq = iaq; }
        // Only a worsening is worth an unscheduled session. An all-clear is good news and can ride
        // along with the next window the device was going to open anyway.
        const AlertTier ptier = (pending && confirmed != 0)
                                  ? Analytics::alertTier(confirmed, (uint16_t)co2) : TIER_CLEAR;
        rtc_alert_tier = (uint8_t)ptier;
        if (pending)
            ESP_LOGW(TAG, "alert pending: class %d (tier %d) — %s", confirmed, (int)ptier,
                     ptier >= TIER_WARNING ? "raising the radio" : "waiting for the next window");
    }

    // The CO2 half of this is spelled out because a zero reading has several possible causes and
    // they are indistinguishable from the number alone: the sensor may not have been due, may
    // have failed to open, may never have been triggered, or may have answered with a zero.
    ESP_LOGW(TAG, "climate: CO2 %.0f ppm | %.1f C | %.0f %% | battery %.0f %% (%.2f V) | air: %s",
             co2, t, h, soc, rtc_volt, txt.c_str());
    if (with_co2) {
        snprintf(rtc_co2_diag, sizeof(rtc_co2_diag),
                 "init=%d shot=%d ok=%d raw=%u waited=%dms", scd_init, scd_shot, scd_ok,
                 raw_co2, waited_ms);
    }
    ESP_LOGW(TAG, "  CO2 path: due=%d %s", with_co2, rtc_co2_diag);
    if (co2_ok) *co2_ok = scd_ok;
    // Who actually answered, not who was due to be asked. The history record leans on this:
    // a field filled from the previous cycle and stamped with this cycle's time is a reading
    // that never happened, and the graphs draw it as data.
    if (bme_ok_out) *bme_ok_out = bme_ok;
    return scd_ok || bme_ok;
}

// Answer the owner's bot commands (/status, /co2, /air, /battery) from the readings this wake holds.
static void answer_telegram_commands(void)
{
    std::string tok, chat;
    NVSManager::getInstance().getString("tg_tok", tok, "");
    NVSManager::getInstance().getString("tg_chat", chat, "");
    if (tok.empty() || chat.empty()) return;
    uint32_t off = 0;
    NVSManager::getInstance().getU32("tg_off", off, 0);
    TgUpdate ups[4];
    // First time round we ask for the latest only: a bot that has been sitting unread
    // would otherwise reply to a week of backlog the moment it is switched on.
    // The command menu is registered once per token. Without it the owner has to know and
    // type "/status"; with it Telegram offers the list beside the text field. Tied to the
    // token rather than to a flag on its own, so a new bot gets its own menu and a
    // reflash does not spend a request repeating what Telegram already stores.
    uint32_t menu_for = 0;
    // The attempt is stamped, not just the success. A bot token that the API refuses —
    // revoked, mistyped, or simply a bad afternoon at Telegram — otherwise re-registered
    // the command menu in every radio window for the life of the device.
    NVSManager::getInstance().getU32("tg_menu", menu_for, 0);
    // The suffix is the menu's version: bumped when the descriptions change, so a bot that already
    // has the old menu is sent the new one once.
    const uint32_t tok_tag = (uint32_t)std::hash<std::string>{}(tok + "#menu-2");
    if (menu_for != tok_tag) {
        const bool okmenu = Telegram::setCommands(tok);
        // Stamped either way. Registering the menu is a convenience, not a duty, and a
        // token the API will not accept is not going to start accepting it on the next
        // window — retrying for ever costs a request in every session, for a bot menu.
        NVSManager::getInstance().setU32("tg_menu", tok_tag);
        if (!okmenu) ESP_LOGW(TAG, "bot command menu not registered — not retrying");
    }

    const int n = Telegram::getUpdates(tok, off ? (int32_t)off : -1, ups, 4);
    for (int i = 0; i < n; i++) {
        // CONSUME FIRST, ANSWER SECOND — and the order is the whole fix.
        //
        // The mark used to be written once, after the loop. Anything that ended the window
        // in between — the radio coming down, a reboot, a failed send — left the mark where
        // it was, so the next session fetched the same updates and answered them again.
        // That is the duplicated /status reply seen on 2026-08-11 (4.58).
        //
        // Committing first turns the guarantee from at-least-once into at-most-once, which
        // is the right way round here: a lost answer costs the owner one more tap of a
        // command they chose to send, while a duplicate looks like the device malfunctioning
        // — and this is a channel whose entire value is that it is believed. The mark also
        // advances for messages from other chats, which are skipped below; without that
        // they would be re-fetched for ever.
        NVSManager::getInstance().setU32("tg_off", (uint32_t)(ups[i].update_id + 1));
        if (ups[i].chat != chat) continue;          // answer nobody else, ever
        const std::string &t = ups[i].text;
        // Every reply is named, because several devices sharing one chat all answer, and says when its
        // readings were taken: the device only answers when it next comes online, so a reply can be
        // much younger than its numbers. The first line is the headline (Telegram shows it bold).
        char reply[384], nbuf[40], w_co2[16], w_gas[16], w_any[16], temp_s[16], pres_s[20], co2_s[24];
        const char *dev = device_name(nbuf, sizeof(nbuf));
        local_stamp(rtc_last_co2, w_co2, sizeof(w_co2));
        local_stamp(rtc_last_meas, w_gas, sizeof(w_gas));
        local_stamp(rtc_last_co2 > rtc_last_meas ? rtc_last_co2 : rtc_last_meas, w_any, sizeof(w_any));
        fmt_temp(rtc_temp, temp_s, sizeof(temp_s));
        fmt_pres(rtc_pres_pa, pres_s, sizeof(pres_s));
        if (rtc_co2 > 0) snprintf(co2_s, sizeof(co2_s), "CO\xE2\x82\x82 %u ppm", (unsigned)rtc_co2);
        else             snprintf(co2_s, sizeof(co2_s), "CO\xE2\x82\x82 no reading yet");
        const bool gas_known = rtc_acc >= 1 && rtc_iaq > 0.0f;
        char air_s[96];
        if (!BSECManager::builtIn())
            snprintf(air_s, sizeof(air_s), "No air-quality index: this firmware is built without Bosch BSEC");
        else if (gas_known)
            snprintf(air_s, sizeof(air_s), "Air quality index %.0f, %s \xC2\xB7 accuracy %u/3",
                     rtc_iaq, Analytics::iaqBandName(Analytics::iaqBand(rtc_iaq)), (unsigned)rtc_acc);
        else
            snprintf(air_s, sizeof(air_s), "Gas sensor warming up \xC2\xB7 accuracy %u/3", (unsigned)rtc_acc);
        char m_co2[32] = "", m_gas[32] = "", m_any[32] = "";
        if (w_co2[0]) snprintf(m_co2, sizeof(m_co2), " \xC2\xB7 measured %s", w_co2);
        if (w_gas[0]) snprintf(m_gas, sizeof(m_gas), " \xC2\xB7 measured %s", w_gas);
        if (w_any[0]) snprintf(m_any, sizeof(m_any), " \xC2\xB7 measured %s", w_any);
        if (t.rfind("/co2", 0) == 0) {
            const AlertConfig acfg = load_alert_config();
            char cmp[64];
            if (rtc_co2 > 0)
                snprintf(cmp, sizeof(cmp), "%s your alert level of %u ppm",
                         rtc_co2 > acfg.co2_level ? "Above" : "Below", (unsigned)acfg.co2_level);
            else
                snprintf(cmp, sizeof(cmp), "No measurement yet");
            snprintf(reply, sizeof(reply), "%s \xC2\xB7 %s\n%s%s", dev, co2_s, cmp, m_co2);
        }
        else if (t.rfind("/air", 0) == 0)
            snprintf(reply, sizeof(reply), "%s \xC2\xB7 %s\n%s%s", dev,
                     !BSECManager::builtIn() ? "No air-quality index"
                     : gas_known ? Analytics::iaqBandName(Analytics::iaqBand(rtc_iaq)) : "Warming up",
                     air_s, m_gas);
        else if (t.rfind("/battery", 0) == 0) {
            const bool usb  = usb_present();
            const bool full = charge_finished(usb);   // the same rule the screen uses
            snprintf(reply, sizeof(reply), "%s \xC2\xB7 Battery %.0f %%\nCell %.2f V \xC2\xB7 %s%s", dev, rtc_soc,
                     rtc_volt, full ? "on USB, charged" : usb ? "on USB, charging" : "on battery", m_any);
        }
        else if (t.rfind("/status", 0) == 0)
            snprintf(reply, sizeof(reply),
                     "%s \xC2\xB7 %s\n%s \xC2\xB7 %s \xC2\xB7 %.0f %%%s\n%s%s\nPressure %s \xC2\xB7 battery %.0f %%",
                     dev, rtc_cls_txt[0] ? rtc_cls_txt : "No reading yet",
                     co2_s, temp_s, rtc_hum, m_co2, air_s, m_gas, pres_s, rtc_soc);
        else
            snprintf(reply, sizeof(reply), "%s\nCommands: /status, /co2, /air, /battery", dev);
        Telegram::send(tok, chat, reply);
        ESP_LOGW(TAG, "telegram command \"%s\" answered", t.c_str());
    }
}

// Decided here and nowhere else: only with the radio up can a decision to notify actually be
// delivered, and the decision commits state (cooldowns, "what did we last say"). Deciding it
// somewhere unreachable would burn the cooldown on a message nobody received.
//
// Allowed in Wi-Fi standalone and, if the owner asks for it, alongside a hub. Silently
// dropping messages the moment someone connects a hub was the wrong call: a person who set up
// Telegram and then switched modes simply stopped being told anything, with no explanation.
// Both at once means two messages for one event, which is their decision to make, not ours.
// Under Matter it stays off regardless — there the device has no route to the internet.
//
// The decision is made whatever the delivery settings say. It used to sit inside the
// Telegram gate, so a device with Telegram switched off decided nothing at all — and the
// hub, which was perfectly able to notify, never learned that anything had happened.
static void notify_owner(void)
{
    const AlertConfig cfg = load_alert_config();

    // Nothing is decided here any more — the measurement tick already did that, without a
    // radio. All that is left is to notice that a message is owed and send it. This is why
    // opening the console no longer produces one: a long press raises the radio, arrives
    // here, finds the confirmed class equal to the one already reported, and says nothing.
    const int confirmed = rtc_alert_confirmed;
    const bool back_to_normal = (confirmed == 0);
    const AlertTier tier = back_to_normal ? TIER_CLEAR
                                          : Analytics::alertTier(confirmed, rtc_alert_co2 ? rtc_alert_co2
                                                                                     : (uint16_t)rtc_co2);
    if (confirmed != rtc_alert.notified_class) {
        // A text still owed from an earlier event goes out first, so the slot is free.
        deliver_alert();
        // Whether this event's message was queued. set_alert() refuses while an earlier text
        // is still owed a delivery; the event is then left unmarked and comes back next
        // window, instead of being marked as told with nothing sent.
        bool taken = true;
        {
            char nbuf[40]; const char *dev = device_name(nbuf, sizeof(nbuf));
            // Say what was measured, what it probably means, and what to do about it. The
            // old message was the class label and three raw numbers, which tells someone
            // who already understands the device nothing they could not read off the glass.
            // What it must never do is name the source: this sensor cannot identify a gas,
            // so "smoke" or "CO" would be a promise the hardware cannot keep (CLAUDE 4.11).
            // One shape for every air message. First line: icon, device, event — Telegram shows it bold
            // and a phone shows it in the notification. Then what was measured and what to do. Last line:
            // the room and the time of the reading behind the message — all of it frozen at that reading
            // (rtc_msg_*), because the message can leave long after it: an advisory waits for its window.
            char msg[640], when[16], temp_s[16], room[72];
            local_stamp(rtc_alert_at, when, sizeof(when));
            fmt_temp(rtc_msg_temp, temp_s, sizeof(temp_s));
            snprintf(room, sizeof(room), "Room %s \xC2\xB7 %.0f %%%s%s", temp_s, rtc_msg_hum,
                     when[0] ? " \xC2\xB7 measured " : "", when);
            const uint16_t co2_then = rtc_alert_co2 ? rtc_alert_co2 : (uint16_t)rtc_msg_co2;
            const bool gas_known = rtc_msg_acc >= 1 && rtc_msg_iaq > 0.0f;
            char gas_now[72] = "";
            if (gas_known)
                snprintf(gas_now, sizeof(gas_now), "air quality index %.0f, %s", rtc_msg_iaq,
                         Analytics::iaqBandName(Analytics::iaqBand(rtc_msg_iaq)));
            if (back_to_normal) {
                // Say what is over — the last message named one thing, and this one closes exactly
                // that — and then how the rest of the air is.
                char over[112], rest[112] = "", co2_s[24];
                if (rtc_msg_co2) snprintf(co2_s, sizeof(co2_s), "CO\xE2\x82\x82 %u ppm", (unsigned)rtc_msg_co2);
                else             snprintf(co2_s, sizeof(co2_s), "CO\xE2\x82\x82 no reading");
                switch (rtc_alert.notified_class) {
                case 1:
                    snprintf(over, sizeof(over), "Stuffy air is over: %s.", co2_s);
                    if (gas_known)
                        snprintf(rest, sizeof(rest), "\nAir quality index %.0f, %s.", rtc_msg_iaq,
                                 Analytics::iaqBandName(Analytics::iaqBand(rtc_msg_iaq)));
                    break;
                case 2: case 3:
                    if (gas_known) snprintf(over, sizeof(over), "Polluted air is over: %s.", gas_now);
                    else           snprintf(over, sizeof(over), "Polluted air is over.");
                    snprintf(rest, sizeof(rest), "\n%s.", co2_s);
                    break;
                default:
                    if (rtc_alert.notified_class == 5)
                        snprintf(over, sizeof(over), "Damp is over: humidity %.0f %%.", rtc_msg_hum);
                    else
                        snprintf(over, sizeof(over), "The air is fine again.");
                    snprintf(rest, sizeof(rest), "\n%s%s%s.", co2_s, gas_known ? " \xC2\xB7 " : "", gas_now);
                    break;
                }
                snprintf(msg, sizeof(msg), "\xE2\x9C\x85 %s \xC2\xB7 Back to normal\n%s%s\n%s", dev, over, rest, room);
                // Unless the only thing that changed is that the owner switched this
                // category off. The bookkeeping is right to close the event, but saying
                // the air is fine again would not be: it has not improved, and nobody
                // asked to be told a comforting thing that is untrue.
                if (rtc_class != 0) {
                    ESP_LOGW(TAG, "category off while class %d stands — closing quietly",
                             rtc_class);
                } else {
                    taken = set_alert("clear", "Back to normal", msg);
                }
            } else {
                const char *icon = (tier >= TIER_WARNING) ? "\xE2\x9A\xA0\xEF\xB8\x8F"
                                                          : "\xF0\x9F\x92\xA8";
                const char *head = "Air quality"; char body[400] = "";
                // Composed from the CONFIRMED class and the readings frozen when it was confirmed, not
                // the live ones: by the time a window opens the air may have moved on, and the message
                // must describe the event it reports.
                switch (confirmed) {
                case 1:
                    head = "Stuffy air";
                    snprintf(body, sizeof(body),
                             "CO\xE2\x82\x82 %u ppm \xE2\x80\x94 above your alert level of %u ppm.\n"
                             "Open a window for a few minutes; it usually drops fast.",
                             (unsigned)co2_then, (unsigned)cfg.co2_level);
                    break;
                case 2:
                case 3: {
                    // Bosch's name and suggested action for the index that tripped it (BME690
                    // datasheet, table 6), then what the other sensor adds. Bosch lists exhaled
                    // breath among what raises the index (datasheet 3.3); breath carries CO2, and
                    // cooking, cleaning products, paint and furniture do not. So the SCD41 says
                    // which of the two to look for, against the owner's own CO2 level.
                    const IaqBand band = Analytics::iaqBand(rtc_alert_iaq);
                    head = Analytics::iaqBandName(band);
                    if (co2_then >= cfg.co2_level)
                        snprintf(body, sizeof(body),
                                 "Air quality index %.0f \xE2\x80\x94 %s.\n"
                                 "CO\xE2\x82\x82 is high too (%u ppm): people and too little fresh air.",
                                 rtc_alert_iaq, Analytics::iaqBandAction(band), (unsigned)co2_then);
                    else
                        snprintf(body, sizeof(body),
                                 "Air quality index %.0f \xE2\x80\x94 %s.\n"
                                 "CO\xE2\x82\x82 is normal (%u ppm), so it is probably not people: look for "
                                 "cooking, cleaning products, paint or furniture.",
                                 rtc_alert_iaq, Analytics::iaqBandAction(band), (unsigned)co2_then);
                    if (confirmed == 3)
                        strncat(body, "\nNot a smoke or fire detector.", sizeof(body) - strlen(body) - 1);
                    break;
                }
                case 5: {
                    head = "Damp";
                    char dp_s[16];
                    fmt_temp(Analytics::getInstance().calculateDewPoint(rtc_msg_temp, rtc_msg_hum), dp_s, sizeof(dp_s));
                    if (std::isfinite(rtc_msg_srh) && std::isfinite(rtc_msg_out)) {
                        // Say what it was judged on. The same room humidity means condensation in
                        // January and nothing at all in July, so the number alone explains nothing.
                        char out_s[16];
                        fmt_temp(rtc_msg_out, out_s, sizeof(out_s));
                        snprintf(body, sizeof(body),
                                 "Dew point %s. With %s outside, a cold wall in this room sits near "
                                 "%.0f %% humidity — mould starts above 80 %%.\n"
                                 "Airing or a little heat helps.", dp_s, out_s, rtc_msg_srh);
                    } else {
                        snprintf(body, sizeof(body),
                                 "Humidity has stayed high for two readings in a row; dew point %s.\n"
                                 "Condensation on cold surfaces and mould risk. Airing or a little "
                                 "heat helps.\n"
                                 "No outdoor reading, so this is judged on the room alone.", dp_s);
                    }
                    break;
                }
                default: snprintf(body, sizeof(body), "%s", rtc_cls_txt); break;
                }
                snprintf(msg, sizeof(msg), "%s %s \xC2\xB7 %s\n%s\n%s", icon, dev, head, body, room);
                taken = set_alert(tier >= TIER_WARNING ? "warning" : "advisory", head, msg);
            }
        }
        if (taken) {
            Analytics::getInstance().alertSent(rtc_alert, confirmed, time(NULL));
            // The channel has now spoken once, so silence from here on means "nothing to
            // report" rather than "possibly broken", and later advisories can go back to
            // waiting for their scheduled window. Set on any message, including an all-clear:
            // what the owner needed was evidence that messages arrive at all.
            { uint8_t f = 0;
              if (!NVSManager::getInstance().getU8("al_first", f, 0) || !f)
                  NVSManager::getInstance().setU8("al_first", 1); }
            // Said in the log, always: silence there looked identical to nothing having
            // happened, which is how a spurious all-clear once went unexplained.
            ESP_LOGW(TAG, "notifying: %s (class %d, tier %d)",
                     back_to_normal ? "BACK TO NORMAL" : "alert", confirmed, (int)tier);
            deliver_alert();   // before the battery block can overwrite this text
        } else {
            ESP_LOGW(TAG, "class %d not reported yet: an earlier message is still owed a "
                          "delivery — trying again next window", confirmed);
        }
    }

    // ---- low battery ---------------------------------------------------------------
    // Separate from the air alerts and not covered by their category switches: this one is
    // about the device itself, and silencing it would only produce a monitor that stopped
    // without saying why. Each level speaks once; they re-arm above 20 %.
    const int bat_lvl = battery_alert_level();
    if (bat_lvl) {
        char nbuf[40]; const char *dev = device_name(nbuf, sizeof(nbuf));
        // The charge as measured, not the threshold it crossed: a device asleep while the cell fell from
        // 18 % to 12 % crosses 15 % and should say 12. The last level names the real cut-off, which is a
        // voltage (battery_too_low(), battery_spec.h), not a percentage.
        const float lock_v = BATTERY_CUTOFF_V;
        char advice[140];
        if (bat_lvl == BAT_WARN_1)
            snprintf(advice, sizeof(advice), "Below 15 %%: worth charging it in the next day or so.");
        else if (bat_lvl == BAT_WARN_2)
            snprintf(advice, sizeof(advice), "Below 10 %%: please charge it soon.");
        else
            snprintf(advice, sizeof(advice),
                     "Below 5 %%: charge it now. At %.2f V the device stops measuring to protect the cell.", lock_v);
        char when[16];
        local_stamp(rtc_last_co2 > rtc_last_meas ? rtc_last_co2 : rtc_last_meas, when, sizeof(when));
        const int soc_now = (int)(rtc_soc + 0.5f);
        char msg[280], head[56];
        snprintf(head, sizeof(head), "Battery %d %%", soc_now);
        snprintf(msg, sizeof(msg),
                 "\xF0\x9F\x94\x8B %s \xC2\xB7 Battery %d %%\n%s\nCell %.2f V%s%s",
                 dev, soc_now, advice, rtc_volt, when[0] ? " \xC2\xB7 measured " : "", when);
        // Marked only if the slot actually took it. These bits silence this threshold for
        // the rest of the discharge, so setting them for an alert that was refused — because
        // an air event ahead of it had not been delivered yet — would lose the battery
        // warning permanently. Refused means "not this window": the level is still low next
        // time, so it comes back on its own.
        //
        // Marked here rather than after a successful send, though: the event happened, and
        // it is in the state message whether or not Telegram is configured. Tying it to one
        // delivery channel meant a hub-only owner was warned about the same 15 % forever.
        if (set_alert(bat_lvl <= BAT_WARN_3 ? "warning" : "advisory", head, msg)) {
            if (bat_lvl <= BAT_WARN_1) rtc_bat_notified |= 1;
            if (bat_lvl <= BAT_WARN_2) rtc_bat_notified |= 2;
            if (bat_lvl <= BAT_WARN_3) rtc_bat_notified |= 4;
        }
        deliver_alert();
    }

    // ---- delivery ------------------------------------------------------------------
    // One place, one text. The hub gets it through the state message published moments
    // from now; Telegram gets it here, if the owner asked for that as well.
    // Gated on a delivery being OWED, not on a text existing. The text lives in RTC memory
    // and therefore exists for ever after the first event; keying off it meant the last
    // message went out again in every radio window and on every long press that opened the
    // console. That was the "notification every five minutes" — the decision above was
    // only half of it, and the more visible half.
    // Anything still owed — a retry from an earlier window whose send failed.
    deliver_alert();
}

// Today's low and high, widened by everything known about the day: the forecast for the hours still to
// come, and what the temperature has actually been. ONE place for both sources — the device's own fetch
// and the hub's message — because a forecast only ever describes the future. By evening a hub's "today"
// is the last few hours, and a device that took it as the day's range showed a high that had already
// fallen by several degrees (found 2026-09-21: Met.no through Home Assistant). The device's own fetch
// always widened; the hub's message used to replace.
// Kept in flash as well: nothing in RTC memory survives the power actually going away, and a morning
// low that has already happened cannot be fetched again. `lo`/`hi` outside -90..70 mean "not known"
// (callers pass the +-999 sentinels).
static void widen_today(time_t now_t, int tz, float lo, float hi, float cur)
{
    NVSManager &nv = NVSManager::getInstance();
    const auto sane = [](float t) { return t > -90.0f && t < 70.0f; };
    // New local day? Start the pair over. This is the ONLY place it is reset.
    const int32_t day = (int32_t)((now_t + tz) / 86400);
    if (rtc_day_stamp == 0) {
        uint32_t saved_day = 0;
        nv.getU32("d_day", saved_day, 0);
        if (saved_day == (uint32_t)day) {
            float slo = 999.0f, shi = -999.0f;
            nv.getFloat("d_lo", slo, 999.0f);
            nv.getFloat("d_hi", shi, -999.0f);
            if (slo < 900.0f && shi > -900.0f) {
                rtc_day_lo = slo; rtc_day_hi = shi; rtc_day_stamp = day;
                ESP_LOGW(TAG, "restored today's min/max from flash: %.0f / %.0f", slo, shi);
            }
        }
    }
    if (day != rtc_day_stamp) {
        rtc_day_stamp = day; rtc_day_lo = 999.0f; rtc_day_hi = -999.0f;
        ESP_LOGW(TAG, "new local day — daily min/max restarted");
    }
    // The forecast for the rest of the day...
    if (sane(lo) && lo < rtc_day_lo) rtc_day_lo = lo;
    if (sane(hi) && hi > rtc_day_hi) rtc_day_hi = hi;
    // ...and what it has actually done so far, which the forecast cannot revise away.
    if (sane(cur)) {
        if (cur < rtc_day_lo) rtc_day_lo = cur;
        if (cur > rtc_day_hi) rtc_day_hi = cur;
    }
    rtc_wx_lo = rtc_day_lo; rtc_wx_hi = rtc_day_hi;

    // Written only when the pair actually moves. It widens a handful of times a day,
    // so this costs a few writes rather than one per fetch.
    float plo = 999.0f, phi = -999.0f; uint32_t pday = 0;
    nv.getFloat("d_lo", plo, 999.0f);
    nv.getFloat("d_hi", phi, -999.0f);
    nv.getU32("d_day", pday, 0);
    if (pday != (uint32_t)day || fabsf(plo - rtc_day_lo) > 0.05f || fabsf(phi - rtc_day_hi) > 0.05f) {
        nv.setU32("d_day", (uint32_t)day);
        nv.setFloat("d_lo", rtc_day_lo);
        nv.setFloat("d_hi", rtc_day_hi);
    }
    ESP_LOGW(TAG, "today so far: min %.0f max %.0f (now %.1f)", rtc_day_lo, rtc_day_hi, cur);
}

// Outdoor weather handed over by the hub on <topic>/weather: `temp` is required, every other field is
// optional, and an absent one keeps its previous value.
static void take_hub_weather(const std::string &wx, time_t wnow)
{
    JsonDocument w;
    if (deserializeJson(w, wx) == DeserializationError::Ok && !w["temp"].isNull()) {
        rtc_wx_temp  = w["temp"] | rtc_wx_temp;
        rtc_wx_feels = w["feels"] | rtc_wx_temp;
        // Today's range is WIDENED by the hub's figures, not replaced by them: see widen_today().
        // Without a clock there is no "today" to keep, and the hub's word is taken as it is.
        if (wnow > 1600000000) {
            float tzh0 = 0; NVSManager::getInstance().getFloat("tz_h", tzh0, 0.0f);
            widen_today(wnow, (int)lroundf(tzh0 * 3600.0f),
                        w["tmin"] | 999.0f, w["tmax"] | -999.0f, rtc_wx_temp);
        } else {
            rtc_wx_lo = w["tmin"] | rtc_wx_lo;
            rtc_wx_hi = w["tmax"] | rtc_wx_hi;
        }
        rtc_wx_hum   = w["hum"] | rtc_wx_hum;
        rtc_wx_wind  = w["wind"] | rtc_wx_wind;
        // The extra strip cells, on the same contract as every other field here: absent KEEPS the
        // previous value rather than blanking it. A hub whose weather integration has no UV simply
        // never sends the key, and the cell goes on showing dashes because nothing ever set it.
        rtc_wx_uv    = w["uv"]    | rtc_wx_uv;
        rtc_wx_dew   = w["dew"]   | rtc_wx_dew;
        rtc_wx_cloud = w["cloud"] | rtc_wx_cloud;
        // isNull() asks whether the key is PRESENT, not whether it is a string. For
        // {"city":5} the key exists, the cast resolves to as<const char*>(), and
        // ArduinoJson returns 0 for a non-string variant -- a NULL straight into %s.
        // There is no safety net underneath: this build has NANO_FORMAT off and the
        // literal "(null)" appears nowhere in the ELF, so newlib's optional guard is
        // compiled out and the formatter reads address 0. That is a panic, on a wake
        // cycle, from one retained message on the owner's own broker -- as likely from
        // a hub template that forgot its quotes as from anyone hostile.
        //
        // Taking the value only when it really is a string also restores the contract
        // the console documents for this topic: every field except temp is optional and
        // an absent one KEEPS its previous value. The old defaults broke that quietly,
        // blanking the description and forcing a sun icon whenever a hub published
        // temperature alone. Copying onto itself is avoided rather than relying on
        // snprintf tolerating overlap, which it is not required to.
        auto takeStr = [](char *dst, size_t cap, JsonVariantConst v) {
            const char *p = v | (const char *)nullptr;
            if (p) snprintf(dst, cap, "%s", p);
        };
        takeStr(rtc_wx_desc, sizeof(rtc_wx_desc), w["desc"]);
        takeStr(rtc_wx_icon, sizeof(rtc_wx_icon), w["icon"]);
        takeStr(rtc_wx_city, sizeof(rtc_wx_city), w["city"]);
        // The outlook, when the hub sends one. Replaced whole rather than merged: it is
        // one look at the days ahead, and a merge could pair this morning's Tuesday with
        // last night's Wednesday. Absent keeps what we had, like every field here. An
        // entry missing any field is skipped rather than guessed at — a temperature that
        // was not sent is not zero degrees.
        const JsonArrayConst fa = w["forecast"].as<JsonArrayConst>();
        if (!fa.isNull()) {
            float tzh = 0; NVSManager::getInstance().getFloat("tz_h", tzh, 0.0f);
            // Days that are not after today are dropped here; with no clock yet there is
            // no "today" to compare with, and the hub is trusted to send the days ahead.
            const int32_t today = (wnow > 1600000000)
                ? (int32_t)(((long long)wnow + lroundf(tzh * 3600.0f)) / 86400) : 0;
            WxDay got[4] = {};
            int n = 0;
            for (JsonObjectConst f : fa) {
                if (n >= 4) break;
                const char *ds = f["date"] | (const char *)nullptr;
                const char *ic = f["icon"] | (const char *)nullptr;
                const float lo = f["tmin"] | 999.0f, hi = f["tmax"] | -999.0f;
                int yy = 0, mo = 0, dd = 0;
                if (!ds || sscanf(ds, "%4d-%2d-%2d", &yy, &mo, &dd) != 3 ||
                    yy < 2020 || yy > 2200 || mo < 1 || mo > 12 || dd < 1 || dd > 31) continue;
                if (!ic || !isdigit((unsigned char)ic[0]) || !isdigit((unsigned char)ic[1])) continue;
                if (lo > 200.0f || hi < -200.0f) continue;
                const int32_t day = days_from_civil(yy, (unsigned)mo, (unsigned)dd);
                if (day <= today) continue;
                got[n].day = day; got[n].lo = lo; got[n].hi = hi;
                got[n].icon[0] = ic[0]; got[n].icon[1] = ic[1];
                got[n].icon[2] = 'd';  got[n].icon[3] = '\0';
                n++;
            }
            memcpy(rtc_fc, got, sizeof(rtc_fc));
            ESP_LOGW(TAG, "outlook from the hub: %d day(s)", n);
        }
        rtc_last_weather = (wnow > 1600000000) ? wnow : rtc_last_weather;
        ESP_LOGW(TAG, "weather from hub: %s %.1f C, %s",
                 rtc_wx_city, rtc_wx_temp, rtc_wx_desc);
    } else {
        ESP_LOGW(TAG, "weather message from the hub was not readable");
    }
}

// The event the hub gets on <topic>/alert: the same words Telegram would have sent, plus the level
// and the device's name, so one automation can serve several devices. Escaping is ArduinoJson's —
// the text is the owner's own device name and free-form words, and a quote in either must not break
// the message.
static std::string alert_event_json()
{
    JsonDocument d;
    std::string dev;
    NVSManager::getInstance().getString("dev_name", dev, "");
    d["device"] = dev.empty() ? "Smart Meteo" : dev;
    d["level"]  = alert_level();
    d["title"]  = rtc_al_title;
    d["text"]   = rtc_al_text;
    std::string out;
    serializeJson(d, out);
    return out;
}

// MQTT mode: the state message, the discovery definitions when they are due, the alert event when one
// is owed, and the weather the hub hands back in the same session.
// Returns whether the broker was actually reached. The caller needs that, and not for a log line:
// in this mode the weather comes from the hub, so "the hub did not answer" is the one fact that
// decides whether this device has to go and fetch the weather itself (radio_window()).
static bool publish_to_hub(void)
{
    publish_globals();      // the payload is built from these, so refresh them first
    // The hub is told what this device is once a day, and again after a restart or a
    // settings change. Not on every window — the definitions are retained, so repeating
    // them buys nothing and costs radio; and not once ever, because a hub reinstalled or a
    // broker wiped would leave the device publishing readings nobody knows how to read.
    const time_t wnow = time(NULL);
    const bool disco = (rtc_mqtt_disco_at == 0) ||
                       (wnow > 1600000000 && wnow - rtc_mqtt_disco_at > 86400);
    std::string wx;
    // The alert as an event, when one is owed. Captured before the session: the acknowledgement
    // settles THIS event, whatever happens to the counter afterwards.
    const uint32_t ev_seq = rtc_al_seq;
    std::string ev;
    if (ev_seq != rtc_al_hub && rtc_al_title[0]) ev = alert_event_json();
    bool ev_acked = false;
    const auto r = MqttPublisher::publishState(MqttPublisher::buildState(), disco, &wx, 8000,
                                               ev.empty() ? nullptr : &ev, &ev_acked);
    if (ev_acked || !rtc_al_title[0]) rtc_al_hub = ev_seq;   // nothing to say is not a debt
    EvtLog::getInstance().setSensor(ES_MQTT, r.ok);
    if (!r.ok) { ESP_LOGE(TAG, "MQTT: %s", r.error ? r.error : "failed");
                 EvtLog::getInstance().event(EVT_NOTE, NOTE_MQTT_FAIL); }
    else if (disco) rtc_mqtt_disco_at = (wnow > 1600000000) ? wnow : 1;

    // Outdoor weather handed over by the hub, which already knows it. Taken in the session
    // that just published, so it costs nothing extra — and it removes the device's last
    // reason to touch the internet: no weather service, no API key, no TLS handshake.
    // Only in this mode; on its own the device still fetches its own (see go_online).
    if (!wx.empty()) take_hub_weather(wx, wnow);
    return r.ok;
}

// The device's own weather, fetched when no hub is handing it over: current conditions, today's low and
// high (kept in flash across a power loss), and the outlook. Two sources (NVS "wx_src", owner 2026-09-17):
// MET Norway by default — free and keyless, one request for everything — and OpenWeatherMap with the
// owner's own key, kept as the alternative.
static void fetch_own_weather(void)
{
    NVSManager &nv = NVSManager::getInstance();
    std::string owm, city;
    nv.getString("owm", owm, "");
    nv.getString("city", city, "");
    uint8_t src = WX_SRC_MET;
    nv.getU8("wx_src", src, WX_SRC_MET);
    if (src == WX_SRC_OWM && owm.empty()) src = WX_SRC_MET;   // no key: MET needs none
    if (city.empty()) {
        ESP_LOGW(TAG, "weather not configured — skipping");
        return;
    }
    const time_t nw = time(NULL);
    WxNow now;
    int tz = 0;
    // The outlook, when this fetch brought one: MET always does, OpenWeatherMap a few times a day.
    bool have_fc = false;
    float flo = 999.0f, fhi = -999.0f;
    WxDay ahead[4] = {};
    int n_ahead = 0;
    esp_task_wdt_reset();

    // MET Norway: coordinates and the device's own zone; MET works in UTC only.
    auto try_met = [&]() -> bool {
        // Coordinates: saved with the city when it was picked from the list; resolved here once when
        // the city was typed by hand (or came from an older configuration).
        float lat = 0, lon = 0;
        const bool have_loc = nv.getFloat("wx_lat", lat, 0.0f) && nv.getFloat("wx_lon", lon, 0.0f) &&
                              (lat != 0.0f || lon != 0.0f);
        if (!have_loc) {
            GeoCity g;
            if (!Weather::locate(city, g)) {
                ESP_LOGW(TAG, "could not find \"%s\" on the map — no MET forecast", city.c_str());
                return false;
            }
            lat = g.lat; lon = g.lon;
            nv.setFloat("wx_lat", lat);
            nv.setFloat("wx_lon", lon);
            ESP_LOGW(TAG, "located %s at %.4f, %.4f", city.c_str(), lat, lon);
        }
        // The local day comes from the device's zone, which the console sets from the city (tz_rule)
        // and keeps across daylight saving (follow_tz_rule()).
        float tz_h = 0; nv.getFloat("tz_h", tz_h, 0.0f);
        tz = (int)lroundf(tz_h * 3600.0f);
        flo = 999.0f; fhi = -999.0f; n_ahead = 0;
        time_t expires = 0; int status = -1;
        const bool r = Weather::met(lat, lon, tz, now, flo, fhi, ahead, 4, n_ahead, expires, status);
        if (expires > nw) rtc_met_expires = expires;
        have_fc = r;
        return r;
    };
    // OpenWeatherMap with the owner's key, by city name — no coordinates needed.
    auto try_owm = [&]() -> bool {
        float tmin = 0, tmax = 0, wt = 0, feels = 0, wind = 0; int hum = 0;
        std::string desc, icon, name;
        if (!Weather::fetch(city, owm, wt, desc, icon, name, tz, feels, tmin, tmax, hum, wind,
                            &now.p_sea)) return false;
        now.temp = wt; now.feels = feels; now.wind = wind; now.hum = hum; now.desc = desc;
        snprintf(now.icon, sizeof(now.icon), "%s", icon.c_str());
        // NB: fetch's own tmin/tmax are deliberately ignored. On the current-weather
        // endpoint they are a momentary spread across the city, not the day's range —
        // for a single point they are usually just the current temperature.
        // What the rest of today is forecast to do, in 3-hour steps. Pulled a few times a
        // day, not on every refresh — but ALWAYS on the first fetch of a new day, because
        // the morning low exists only as a future forecast entry and only for a few hours.
        const int32_t day = (int32_t)((nw + tz) / 86400);
        if (day != rtc_day_stamp || rtc_last_fc == 0 || (nw - rtc_last_fc) >= FORECAST_SEC) {
            esp_task_wdt_reset();
            flo = 999.0f; fhi = -999.0f; n_ahead = 0;
            // The same request as before, asked for five days instead of one: the outlook
            // row costs no extra window and no extra handshake.
            have_fc = Weather::forecast(city, owm, tz, flo, fhi, ahead, 4, n_ahead);
        }
        return true;
    };

    // MET's terms: nothing before the Expires time of its last answer. The cached numbers stand, and this
    // is not a failure — so neither is it a reason to ask the other source.
    if (src == WX_SRC_MET && rtc_met_expires > nw && rtc_met_expires - nw < 6 * 3600) {
        ESP_LOGW(TAG, "MET data valid for %ld s more — not asking again", (long)(rtc_met_expires - nw));
        return;
    }
    // The chosen source first; when it fails, the other one, if it can be asked. Autonomy is the point
    // of this device (owner, 2026-09-17): one provider being down must not blank the weather. MET needs no
    // key, so it can always stand in; OpenWeatherMap only when the owner stored a key.
    uint8_t used = src;
    bool ok = (src == WX_SRC_MET) ? try_met() : try_owm();
    if (!ok) {
        const bool other_ok = (src == WX_SRC_MET) ? !owm.empty()
                                                  : !(rtc_met_expires > nw && rtc_met_expires - nw < 6 * 3600);
        if (other_ok) {
            used = (src == WX_SRC_MET) ? WX_SRC_OWM : WX_SRC_MET;
            ESP_LOGW(TAG, "%s failed — trying %s", src == WX_SRC_MET ? "MET Norway" : "OpenWeatherMap",
                     used == WX_SRC_MET ? "MET Norway" : "OpenWeatherMap");
            esp_task_wdt_reset();
            ok = (used == WX_SRC_MET) ? try_met() : try_owm();
        }
    }

    if (!ok) {
        // Remembered, because the cached values are about to make this look fine. The web
        // console has a notice for exactly this and it could never appear: the only writer
        // of wx_state set it from "is there a city in the cache", so a device that had
        // failed to reach the weather service for a week still reported "ok" and showed
        // last week's temperature under a freshness label that kept counting up.
        rtc_wx_failed = true;
        ESP_LOGW(TAG, "weather fetch failed — keeping the cached values");
        return;
    }

    rtc_wx_failed = false;
    const float wt = now.temp;
    rtc_wx_temp = wt; rtc_wx_feels = now.feels; rtc_wx_wind = now.wind;
    rtc_wx_hum = now.hum;
    rtc_wx_uv = now.uv; rtc_wx_dew = now.dew; rtc_wx_cloud = now.cloud;
    // Not shown anywhere: this one teaches the device its own altitude, which is what lets the
    // barometric plaque judge the LEVEL and not just the rate. OWM leaves the sentinel, so only a
    // MET report trains it.
    baro_learn_factor(now.p_sea);

    if (have_fc) {
        // Replaced whole, as the hub's is. A response with no days ahead leaves the
        // old outlook to age out by date rather than blanking the row at once.
        if (n_ahead > 0) memcpy(rtc_fc, ahead, sizeof(rtc_fc));
        rtc_last_fc = time(NULL);
    }
    widen_today(time(NULL), tz, have_fc ? flo : 999.0f, have_fc ? fhi : -999.0f, wt);
    snprintf(rtc_wx_city, sizeof(rtc_wx_city), "%s", city.c_str());
    snprintf(rtc_wx_desc, sizeof(rtc_wx_desc), "%s", now.desc.c_str());
    snprintf(rtc_wx_icon, sizeof(rtc_wx_icon), "%s", now.icon);
    rtc_last_weather = time(NULL);
    // OpenWeatherMap names the city's offset, daylight saving included. MET does not; there the zone
    // comes from the console's rule (follow_tz_rule()).
    if (used == WX_SRC_OWM) nv.setFloat("tz_h", tz / 3600.0f);
    ESP_LOGW(TAG, "weather (%s): %s %.1f C, %s", used == WX_SRC_MET ? "MET Norway" : "OpenWeatherMap",
             city.c_str(), wt, now.desc.c_str());
}

// Join the home network, set the clock, and refresh the outdoor weather if it is stale.
// Set for a wake whose long press opens the console (radio_window); go_online() then leaves the
// automatic install to the moment the console closes.
static bool s_console_session = false;

// The automatic install, and the restart into the new image when it went in.
static void install_update_if_due(void)
{
    if (!OtaInstall::due() || OtaInstall::run() != OtaInstall::Result::Installed) return;
    // The hand-off, and THIS is the place it belongs (4.53). BSEC is initialised on this wake,
    // unlike in the setup window, so the snapshot describes something real; and all six keys go
    // together, because a fresh state blob beside an hour-old clock hands the next image a
    // timeline running backwards and it drops the run-in this was meant to preserve.
    EvtLog::getInstance().event(EVT_OTA, 0xFFFFFFFFu);   // installed; the reboot follows
    smart_meteo_ota_handoff();
    ESP_LOGW(TAG, "restarting into the new image");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void go_online(bool weather_due)
{
    // Slot 0 still answers "is there a network at all": wifi_slots.h keeps it as the key every
    // other reader has always used, and an empty slot 0 means the list is empty.
    std::string ssid;
    NVSManager::getInstance().getString("ssid", ssid, "");
    if (ssid.empty()) return;
    // An owner who chose to run without a network chose it for every path, not just the scheduled
    // ones. An air alert or a flat cell would otherwise still spend a full join budget — three
    // seconds of fast-connect and ten of scanning — on a device that has nowhere to send anything.
    { uint8_t nw = 0; NVSManager::getInstance().getU8("no_wifi", nw, 0); if (nw) return; }

    // The driver itself can decline to load — ESP_ERR_NO_MEM is the one that actually happens,
    // and it happens on exactly the wakes where memory is already tight. That is a failed window,
    // not a failed device: it takes the same backoff as a failed join, so the schedule stands
    // back instead of retrying every minute into a heap that has not changed.
    // connectBest(), not connectSTA(ssid): the device remembers up to three networks and joins the
    // one that is actually in the air (wifi_slots.h). With one saved network this IS connectSTA —
    // the everyday path is unchanged, and that is deliberate, because the radio is already the
    // second-largest item in the power budget after the panel.
    if (!WiFiManager::getInstance().init() ||
        !WiFiManager::getInstance().connectBest()) {
        rtc_wifi_fail = true;
        // Stand further back after each failure. A failed join used to cost the schedule nothing:
        // this function returns here, and the only line that advances the schedule sits further
        // down, past the weather fetch. "Due" therefore never stopped being true and the radio came
        // up on EVERY wake — once a minute with the live clock — each attempt burning a 3 s
        // fast-connect and then a 10 s full scan. A router switched off for a weekend was enough to
        // flatten a cell that should have lasted months, and the only symptom was the !wifi mark on
        // the glass. wifi_manager.cpp promises "the profile dispatcher applies a backoff"; that
        // dispatcher belonged to the previous cycle, and nothing here replaced it.
        if (rtc_wifi_fails < 6) rtc_wifi_fails++;
        static const uint16_t BACKOFF[6] = { 1, 2, 5, 15, 30, 60 };   // wakes to sit out
        rtc_wifi_skip = BACKOFF[rtc_wifi_fails - 1];
        // With a working clock the ordinary schedule is the better lever of the two, so advance it
        // as well: retries then follow the cadence the owner chose instead of a count of wakes,
        // whose length changes between day and night (CLAUDE.md §4.19 — once sleep is variable, any
        // schedule measured in ticks is a bug waiting to happen).
        const time_t nw = time(NULL);
        if (nw > 1600000000) rtc_last_wx_try = nw;
        ESP_LOGW(TAG, "Wi-Fi join failed (%u in a row) — radio down for the next %u wake(s)",
                 (unsigned)rtc_wifi_fails, (unsigned)rtc_wifi_skip);
        return;
    }
    rtc_wifi_fail = false;
    rtc_wifi_fails = 0;
    rtc_wifi_skip  = 0;
    // The network has demonstrably been joined, which is the whole meaning of "wifi_ok". It was
    // written in exactly one place — the setup window's connect branch — so a network saved from a
    // console reached over the home LAN (which deliberately does not attempt the join there, to
    // avoid cutting the page off) stayed marked unverified for ever, and the log said "saved but
    // never joined" about a network the device was plainly on. NVS takes a write only when the
    // value actually changes, so this is not a write per window.
    {
        uint8_t ok8 = 0;
        NVSManager::getInstance().getU8("wifi_ok", ok8, 0);
        if (!ok8) {
            NVSManager::getInstance().setU8("wifi_ok", 1);
            ESP_LOGW(TAG, "network joined from the working cycle — marked verified");
        }
    }
    // Every stage below can block: a TLS handshake, a broker that accepts the connection
    // and then says nothing, two weather calls, an NTP exchange. Each has its own timeout
    // and none of them knew about the other, so a bad afternoon could add past the sixty
    // seconds the watchdog allows and reboot a device that was merely being patient.
    esp_task_wdt_reset();
    ESP_LOGW(TAG, "online as %s", WiFiManager::getInstance().getIPAddress().c_str());

    // Announce the name the screen prints. Without this the band on the glass says
    // "smartmeteo.local" and the browser cannot resolve it — the device would be promising an
    // address that does not exist, and only the raw IP would work.
    static bool mdns_up = false;
    if (!mdns_up && mdns_init() == ESP_OK) {
        mdns_hostname_set("smartmeteo");
        mdns_instance_name_set("Smart Meteo");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        mdns_up = true;
        ESP_LOGW(TAG, "mDNS up: smartmeteo.local");
    }

    // Matter mode: this window exists only because somebody held the button for the console. The hub
    // owns the notifications and this screen has no weather, so the home network is used for the two
    // things Thread cannot give (CLAUDE.md, 2026-09-16: no route out of the Thread network) — the
    // clock, synced on every such session because the hub may never set it, and the updates.
    const bool matter = MatterMode::selected();
    if (matter) {
        ClockSync::getInstance().forceClockSync();
        ESP_LOGW(TAG, "Matter mode console session: clock and updates only");
    }

    // Read once for the whole window. Nothing in it can change them: the web server that writes settings
    // runs only in a setup window or the console, and neither is open while this runs.
    uint8_t wl_mode = 0, tg_en = 0;
    NVSManager::getInstance().getU8("wl_mode", wl_mode, 0);
    NVSManager::getInstance().getU8("tg_en", tg_en, 0);
    if (matter) { wl_mode = 2; tg_en = 0; }   // no Telegram and no MQTT from here

    // ---- inbound commands ------------------------------------------------------------------
    // One cheap HTTPS GET per radio window. On battery that means an answer arrives at the next
    // scheduled window rather than instantly — holding Wi-Fi up to poll would cost weeks of
    // runtime, and a webhook is impossible behind a home router.
    if (wl_mode <= 1 && tg_en) answer_telegram_commands();

    // ---- notifications -------------------------------------------------------------------
    if (!matter) notify_owner();

    // ---- security notices ------------------------------------------------------------------
    // Outside notify_owner() on purpose. These two blocks were written inside it, in the branch
    // that runs only when an AIR ALERT has just been confirmed and reported — so in a home with
    // clean air neither ever ran. The "firmware updated" notice waited for an unrelated alert that
    // might be weeks away and would then arrive describing an update nobody remembered, and any
    // notice not delivered inside the console window was never retried at all. Reviewed 2026-09-18.
    if (rtc_sec_updated) {
        char t[192];
        // The running image's own version string, not a macro: it is what the console and
        // `esptool image_info` report, so the three always agree.
        const esp_app_desc_t *ad = esp_app_get_description();
        snprintf(t, sizeof(t), "the firmware was updated to %s. Nothing else changed: settings, "
                               "history and calibration are as they were.",
                 ad ? ad->version : "a new version");
        smart_meteo_security_note(t);
        rtc_sec_updated = 0;
    }
    deliver_security_note();

    // The state message goes out AFTER the decision above, so a fresh alert reaches the hub
    // in the same session that discovered it rather than waiting half an hour for the next.
    // ---- MQTT ------------------------------------------------------------------------------
    // One retained message per radio window, then the connection goes away with the rest of the
    // session. Publishing is the whole job in this mode: the hub owns the alerting, the history
    // and the automations, and the device is a sensor that speaks and goes back to sleep.
    esp_task_wdt_reset();   // network stages block; keep the watchdog informed
    bool hub_reached = false;
    if (wl_mode == 1) hub_reached = publish_to_hub();
    // No hub, no debt. rtc_al_seq counts every confirmed alert whatever the mode, while rtc_al_hub
    // only ever moves inside publish_to_hub() — so in plain Wi-Fi or in Matter the two drift apart
    // and the FIRST window after somebody turns MQTT on republishes the newest of those as a fresh
    // event, straight to every phone through the automation this console writes. The alert carries
    // its own frozen timestamp, so the date is visible on inspection, but the push is not.
    else rtc_al_hub = rtc_al_seq;

    ClockSync::getInstance().maintainClock();     // NTP: date and time
    time_t now = time(NULL);
    ESP_LOGW(TAG, "clock %s", now > 1600000000 ? "synced" : "NOT synced");

    // Not in MQTT mode: the hub just handed the weather over in the session above, and going out
    // to a weather service as well would spend the most expensive operation this device has on
    // numbers it already has. In Wi-Fi standalone there is no hub, so it fetches its own.
    // The hub is the preferred source, not the only one. It is normally there — Home Assistant
    // sets up a weather service by itself during onboarding — but it can be removed, or never
    // given a location, and then nothing would ever publish. Without this the device would sit on
    // whatever it last knew, showing stale numbers under a freshness tag that quietly ages.
    // So: defer to the hub while it is speaking, and go and fetch for itself once it has been
    // silent for six hours. That is far longer than any normal gap and short enough to notice.
    //
    // "Silent" and "not there" are different, and until 2026-10-01 this code could not tell them
    // apart — it only asked how old the last hub weather was. Carry the device away from its home
    // network in this mode and the broker is simply unreachable: nothing will ever publish, yet the
    // six-hour clock still had to run out before the device would look after itself. Found in the
    // field — the owner took it to an office on a phone hotspot, pressed the button, and watched a
    // two-hour-old forecast refuse to move with no explanation anywhere they could see.
    //
    // The device already knew: publish_to_hub() has the broker's answer in its hand twenty lines
    // above. So the deferral now requires BOTH — the hub answered, and its weather is fresh. A
    // broker that did not answer means this window brings nothing from the hub, and fetching our
    // own is simply what the mode is for. The six hours stay for the case they were written for:
    // the broker is there and talking, but nobody is publishing weather into it.
    const time_t wnow2 = time(NULL);
    const bool hub_weather_stale = (rtc_last_weather == 0) ||
                                   (wnow2 > 1600000000 && wnow2 - rtc_last_weather > 6 * 3600);
    if (matter) {
        // No weather in Matter mode: the screen has no outdoor block to put it in.
    } else if (weather_due && wl_mode == 1 && !hub_weather_stale && hub_reached) {
        ESP_LOGW(TAG, "weather comes from the hub in this mode — not fetching");
        rtc_last_wx_try = (wnow2 > 1600000000) ? wnow2 : rtc_last_wx_try;
    } else if (weather_due) {
        if (wl_mode == 1)
            // %s, not the strings themselves: ESP_LOGW builds its format by concatenating string
            // literals, so a ternary cannot stand where the format goes.
            ESP_LOGW(TAG, "%s", hub_reached ? "the hub has not published weather for hours — fetching it ourselves"
                                            : "the hub did not answer — fetching the weather ourselves");
        // Stamp the attempt before doing anything else. Scheduling used to key off the last
        // SUCCESS, so an unconfigured or unreachable weather service meant "still due" on every
        // single tick — and the radio came up every five minutes to fail again.
        rtc_last_wx_try = time(NULL);
        fetch_own_weather();
    }
    // LAST, and only inside a window that something else already opened. The check costs one TLS
    // handshake and about half a kilobyte — some 2 uA averaged over a day — while raising the radio
    // for it on its own schedule would cost forty times that. Nothing above may be skipped for it:
    // the clock, the readings and the weather are what the owner is paying this window for.
    if (OtaCheck::due(time(NULL))) {
        OtaCheck::run();
        // The check is the half nobody can see: it happens once a day inside a window opened for
        // something else, and when there is nothing to install it is silent by design.
        EvtLog::getInstance().event(EVT_OTA, OtaCheck::available_seq);
    }

    // Installing is the very last thing any window does, and it ends the wake by rebooting — so it
    // must not be able to cost the owner the readings, the clock or the weather this session exists
    // for. Everything above has already happened by the time we get here. Not when the owner held the
    // button for the console: the restart would take the console with it, and in Matter mode a console
    // session is the only time an update can arrive. That install waits until the console closes.
    if (s_console_session) {
        if (OtaInstall::due()) ESP_LOGW(TAG, "an update is waiting — installed when the console closes");
        return;
    }
    install_update_if_due();
}

// Everything gathered: draw the screen ONCE, with the whole picture on it.
// Returns what was done to the panel, because the sleep sequence depends on it:
//   0 = not touched   1 = partial refresh   2 = full refresh
static int draw_dashboard(EPaper &epd, bool full, bool night, bool offline,
                          bool live_clock, const char *addr = nullptr)
{
    // Outside the panel's working temperature there is no valid waveform for it to run. Driving it
    // anyway gives half-finished frames at best, and repeated out-of-range updates are what damages
    // the film — so the recurring repaints stop and the glass simply keeps the last picture, which
    // costs it nothing. A cold hallway, an unheated holiday house or a winter delivery are ordinary
    // places for this device to end up in.
    //
    // Room temperature comes from the CO2 sensor, not the gas one: the gas sensor's own 320 C
    // heater sits beside its thermometer and reads about five degrees high, which near a freezing
    // threshold is the difference between guarding and not (see CLAUDE.md 4.10).
    //
    // Zero means "never measured" here, the same sentinel the reset paths write, so a device that
    // has not read a sensor yet is not mistaken for a device at exactly 0 C and left blank forever.
    // The rare static screens — logo, setup QR, low-battery notice — deliberately do NOT check
    // this: they happen once in a long while rather than a thousand times a day, and a user staring
    // at a blank panel with no explanation is worse than one out-of-range frame.
    const float room = rtc_temp;
    const bool have_temp = (room != 0.0f && room > -40.0f && room < 85.0f);
    if (have_temp && (room < INK_MIN_C || room > INK_MAX_C)) {
        if (!rtc_ink_blocked) {
            rtc_ink_blocked = true;
            ESP_LOGE(TAG, "room %.1f C is outside the panel's %g..%g C range — "
                          "screen updates paused, the last picture stays", room, INK_MIN_C, INK_MAX_C);
        }
        return 0;
    }
    if (rtc_ink_blocked) {
        rtc_ink_blocked = false;
        rtc_prev_valid = false;      // what is on the glass is old and unknown: repaint in full
        ESP_LOGW(TAG, "room %.1f C is back in range — resuming with a full refresh", room);
    }

    float tz = 0; NVSManager::getInstance().getFloat("tz_h", tz, 0.0f);
    // The units the user picked. The web console has honoured these for a while; the glass never
    // did, so switching to Fahrenheit changed the page and left the device showing Celsius.
    // Readings are stored and logged in Celsius and hPa either way — this converts for display
    // only, which is why the history and the API stay comparable across devices.
    uint8_t u_t = UNIT_TEMP_DEFAULT, u_p = UNIT_PRES_DEFAULT;
    NVSManager::getInstance().getU8("u_temp", u_t, UNIT_TEMP_DEFAULT);   // 0 = C, 1 = F
    NVSManager::getInstance().getU8("u_pres", u_p, UNIT_PRES_DEFAULT);   // 0 = hPa, 1 = mmHg
    auto Tdisp = [&](float c) { return u_t ? c * 9.0f / 5.0f + 32.0f : c; };
    // Rounded to the NEAREST minute, not truncated. The device deliberately wakes a second or so
    // before the minute so the pixels settle exactly on it — which means the clock here is still
    // reading the previous minute, and truncating would put a permanently late time on the glass.
    // Rounding also covers the other side: a measuring tick that overruns lands just after the
    // minute, and still shows the minute it belongs to.
    const time_t now = ((time(NULL) + 30) / 60) * 60 + (time_t)(tz * 3600);
    struct tm tmv; gmtime_r(&now, &tmv);

    Frame cur = {};
    if (addr && addr[0]) {
        snprintf(cur.setup, sizeof(cur.setup), "%s", addr);
        // The console asks for this password, and this band is the only place the owner can read it
        // on the home-network path. Printed while the window is open and gone the moment it closes,
        // so it is not on the glass for a visitor to read at leisure.
        // The password ALONE, no label: the screen draws it three times the size of the address now
        // (the owner could not read it otherwise, hardware 2026-09-19), and at that size the word
        // "password" does not fit beside it — it was pushing both ends of the row off the band.
        snprintf(cur.setup2, sizeof(cur.setup2), "%s", WiFiManager::apPassword());
    }
    // At night the clock is replaced by a moon. Not decoration: without minute wakes the time
    // cannot be kept current, and a frozen clock reading 23:04 at four in the morning is a lie.
    // The readings themselves stay live — the night saves wakes, not data (CLAUDE.md 4.14).
    cur.night = night;
    // Ages are measured against the RAW clock, not the rounded local one used for the display.
    const time_t wall = time(NULL);
    // The fallback used to be the literal "now" — and fmtAge maps a young age to that same word, so
    // an age nobody could compute was indistinguishable from a fresh reading. It matters most for
    // the weather: rtc_last_weather is only stamped once the clock passes the sanity floor, so on a
    // LAN with no internet in hub mode (NTP fails, the hub's retained message still arrives) the
    // city block would carry a permanent "now" over a message of unbounded age — and the six-hour
    // self-fetch fallback is disabled in exactly that state. Say "--" and mean it.
    if (wall > 1600000000 && rtc_last_co2 > 0) fmtAge(wall - rtc_last_co2, cur.fresh, sizeof(cur.fresh));
    else                                       snprintf(cur.fresh, sizeof(cur.fresh), "--");
    if (wall > 1600000000 && rtc_last_weather > 0)
        fmtAge(wall - rtc_last_weather, cur.wxFresh, sizeof(cur.wxFresh));
    else snprintf(cur.wxFresh, sizeof(cur.wxFresh), "--");

    // The offline layout: chosen when the radio is switched off for good, or when there is simply
    // no weather to show. Leaving the outdoor block in place would reserve a third of the screen
    // for a city name that is never coming, and the graph is the thing worth the space instead.
    cur.noNet = (offline || !rtc_wx_city[0]) ? 2 : 0;
    cur.presMode = rtc_pres_mode; cur.ptrend = rtc_ptrend;
    cur.usb = usb_for_frame(rtc_prev_valid ? rtc_prev.usb : usb_present());
    // Has the charger finished? Two things say so together: the cell is at its terminating voltage
    // and the charge is no longer moving. That is the same moment the charger's own LED goes out,
    // so the screen and the board agree instead of contradicting each other.
    //
    // The gauge is believed about the voltage but not about the last couple of percent: it works
    // from voltage rather than counting charge, and a percent or two of error at full scale is
    // within its specification. Reading 98 % on a cell the charger has just declared full is that
    // error, not a fault — so the fact wins and the estimate is rounded up to it.
    // TP4056 datasheet: float voltage 4.137–4.263 V, termination at C/10. A 4.15 V bar never read
    // "done" on a charger regulating at the low end, so the bar sits below the tolerance band.
    const bool charge_done = charge_finished(cur.usb);
    // A bolt means current is actually flowing. On a finished charge it is not, and a bolt that
    // never goes out tells the user nothing — which is what charge_done above answers, from two
    // witnesses (the charger's own STDBY# on v2, near-full volts with a stalled rate on v1).
    //
    // IT USED TO ALSO REQUIRE rtc_soc < 99, AND THAT WAS A COLLISION WITH THE CEILING ADDED
    // 2026-10-08: while the charger has not finished, the shown percentage is pinned at EXACTLY
    // CHARGING_MAX_PCT = 99 (battery_source.h). So the bolt went out the moment the cell got near
    // full and stayed out for the whole rest of the charge — and that rest is most of it: constant
    // current ends at about 42 % of the charge delivered, the falling-current tail runs some three
    // and a half hours (journal 2026-10-06). The owner saw a plug, 99 %, no bolt, and the charger's
    // LED still lit (2026-10-13). A percentage was never the right witness for "is current
    // flowing"; the two rules simply have to agree on who answers that, and charge_done does.
    const PortalScreen::Battery shown = shown_battery(cur.usb, charge_done);
    cur.chg = shown.charging;
    // While charging, low battery is a problem already being solved — no need to shout about it.
    cur.lowbat = (rtc_soc > 0.0f && rtc_soc < 15.0f && !cur.usb);
    cur.wifiFail = rtc_wifi_fail;

    // The graph comes from the LAST records BY POSITION, not from a [now-24h, now] window. Every
    // stored CO2 value is real, and tying the query to the wall clock meant records stamped before
    // a clock correction fell outside the window and the line kept emptying itself. The axis
    // labels come from the records' own timestamps, so they describe what is actually drawn.
    HistStore::getInstance().init();
    struct GraphFill { uint16_t *arr; int n, max; uint32_t t0, t1; } gf = { cur.g_co2, 0, 48, 0, 0 };
    // The window the owner asked for, in hours. Reading the last 48 records instead — which is what
    // this used to do — made the graph mean something different in every profile: four hours in
    // Performance, twelve in Optimal, because the records are further apart. A span in hours means
    // the same thing everywhere, which is the only way a label like "24H" can be honest.
    uint8_t span_h = 24;
    NVSManager::getInstance().getU8("g_span", span_h, 24);
    if (span_h < 1 || span_h > 168) span_h = 24;
    const time_t g_now = time(NULL);
    auto gfill = [](const HistRec &r, void *ctx) {
        GraphFill *g = (GraphFill *)ctx;
        if (g->n < g->max) { g->arr[g->n++] = r.co2; if (!g->t0) g->t0 = r.t; g->t1 = r.t; }
    };
    // Only records that actually carry a CO2 reading. The log gains one entry per gas tick — every
    // five minutes — but the CO2 sensor follows the profile, so on the default Optimal two records
    // in three hold the "no reading" zero. Plotted, those are not gaps: they are a line along the
    // bottom of the chart. Worse, the decimation stride for a 24 h window is six, which shares its
    // factor with the three-record period, so every sampled point lands on the same phase — in two
    // window positions out of three the ENTIRE line would have been zero, on the default profile,
    // on the surface most owners ever look at. The browser has filtered this since this morning;
    // the glass had not.
    static const auto hasCo2 = [](const HistRec &r) { return r.co2 != 0; };
    if (g_now > 1600000000)
        HistStore::getInstance().query((uint32_t)(g_now - (time_t)span_h * 3600),
                                       (uint32_t)g_now, 48, gfill, &gf, hasCo2);
    // Falls back to plain position when the window comes back nearly empty: a device that has just
    // been switched on, or one whose clock has only now been set, has readings worth drawing even
    // though none of them fall inside the last 24 hours by their timestamps.
    if (gf.n < 2) {
        gf.n = 0; gf.t0 = 0; gf.t1 = 0;
        HistStore::getInstance().query(0, 0, 48, gfill, &gf, hasCo2);
    }
    cur.g_n = (uint8_t)gf.n; cur.g_t0 = gf.t0; cur.g_t1 = gf.t1;

    // No clock, no date: 1970 on the glass would be a confident wrong statement. Empty fields are drawn
    // as dashes (fill() below). In Matter mode this is the normal state until the hub or a console
    // session sets the time.
    const bool have_clock = wall > 1600000000;
    const int dd = tmv.tm_mday % 100, mo = (tmv.tm_mon + 1) % 100, yy = (tmv.tm_year + 1900) % 10000;
    if (have_clock) snprintf(cur.date, sizeof(cur.date), "%02d.%02d.%04d", dd, mo, yy);
    // Without a live clock the device sleeps between readings, so the time on the glass can only
    // ever be the time of the last one. Said plainly rather than left to look like a stopped clock.
    cur.updClock = !live_clock && !night;

    // At night the moon takes the clock's place, so the time is deliberately left out of the frame.
    // Carrying it anyway would make the frame differ on every single wake, and the "nothing
    // changed" check below — the thing that stops the panel being redrawn for no reason — would
    // never once fire.
    if (!night && have_clock) {
        // With a live clock this is the current minute; without one it is when the reading was
        // actually taken, which is what the UPD prefix promises.
        struct tm show = tmv;
        if (cur.updClock && rtc_last_meas > 0) {
            const time_t lt = rtc_last_meas + (time_t)(tz * 3600);
            gmtime_r(&lt, &show);
        }
        snprintf(cur.time, sizeof(cur.time), "%02d:%02d", show.tm_hour % 24, show.tm_min % 60);
    }
    // Exactly zero is the "never measured" sentinel, not a measurement. The SCD41 reports nothing
    // at all when it fails (§4.23, and it has failed on this very unit), and neither 0 ppm nor
    // 0.0 °C nor 0 %RH is a state a room can be in. The draw site turns an empty field into the
    // dashes §4.14 asks for; printing the zero instead produced a confident, wrong reading — the
    // same mistake the pressure channel below already avoids.
    if (rtc_co2 > 0) snprintf(cur.co2, sizeof(cur.co2), "%u", (unsigned)rtc_co2);
    else             cur.co2[0] = '\0';
    cur.err = (rtc_co2_fails >= 3);
    cur.tzmin = (int16_t)lroundf(tz * 60.0f);   // travels with the frame, not with NVS

    if (rtc_temp != 0.0f) snprintf(cur.temp, sizeof(cur.temp), "%.1f", Tdisp(rtc_temp));
    else                  cur.temp[0] = '\0';
    if (rtc_hum > 0.0f) snprintf(cur.hum, sizeof(cur.hum), "%.0f", rtc_hum);
    else                cur.hum[0] = '\0';
    // A barometer reading outside 300..1100 hPa is not a low reading, it is a dead channel — one
    // of these units has one (CLAUDE.md 4.17). Show that it is missing rather than print a number.
    if (rtc_pres_pa > 30000.0f)
        snprintf(cur.pres, sizeof(cur.pres), "%.0f",
                 u_p ? rtc_pres_pa * 0.00750062f : rtc_pres_pa / 100.0f);
    else snprintf(cur.pres, sizeof(cur.pres), "--");
    snprintf(cur.presU, sizeof(cur.presU), "%s", u_p ? "mmHg" : "hPa");
    snprintf(cur.cal,  sizeof(cur.cal),  "%u/3", rtc_acc);
    cur.batt = shown.percent;
    cur.outT = (int)lroundf(Tdisp(rtc_wx_temp));

    // The air verdict on the glass (owner, 2026-09-14): the class in the screen's short words, over
    // seven cells, one per Bosch class, filled up to the class of the index. Telegram, MQTT and the
    // console carry the full names (rtc_cls_txt). Stuffy and Damp come from the SCD41 and take the
    // word, while the cells go on showing the gas sensor, so the block shows both sensors at once.
    // Without gas accuracy, or with nothing classified yet (a fresh device), the word is Warm-up and
    // the cells stay empty — unknown air is not good air.
    const int cls  = (rtc_class > 0 && rtc_class < 6) ? rtc_class : 0;
    const int airc = (rtc_air_class > 0 && rtc_air_class < 4) ? rtc_air_class : 0;
    const bool gas_ready = rtc_cls_txt[0] && rtc_acc >= 1;
    const IaqBand band = Analytics::iaqBand(rtc_iaq);
    // Damp is no longer allowed to take this word (owner, 2026-10-02): it says nothing about the
    // air and it used to hide the gas verdict, so a room that was damp AND polluted reported only
    // the moisture. It now lives in the humidity block, beside the number it was read from.
    snprintf(cur.aq, sizeof(cur.aq), "%s",
             airc == 1 ? "Stuffy" : gas_ready ? Analytics::iaqBandShort(band)
             : BSECManager::builtIn() ? "Warm-up" : "No IAQ");
    cur.aqLevel = gas_ready ? (int8_t)band : (int8_t)-1;
    // Inverting the block is the screen's way of raising its voice, so it is kept for the warnings:
    // polluted air above IAQ 200, stuffy and damp. «Moderately polluted» is not one of them: Bosch's
    // advice there is more ventilation, and a black rectangle across the panel reads as alarm — the
    // wrong feeling for a reading that will be back to normal after some airing.
    cur.aqInv   = (airc == 1 || airc == 3);
    cur.humDamp = (cls == 5);

    if (rtc_wx_city[0]) {
        snprintf(cur.city, sizeof(cur.city), "%s", rtc_wx_city);
        snprintf(cur.desc, sizeof(cur.desc), "%s", rtc_wx_desc);
        snprintf(cur.icon, sizeof(cur.icon), "%s", rtc_wx_icon);
        const bool mmok = (rtc_wx_lo < 900.0f && rtc_wx_hi > -900.0f);
        if (mmok) {
            snprintf(cur.lo, sizeof(cur.lo), "%+d", (int)lroundf(Tdisp(rtc_wx_lo)));
            snprintf(cur.hi, sizeof(cur.hi), "%+d", (int)lroundf(Tdisp(rtc_wx_hi)));
        } else { snprintf(cur.lo, sizeof(cur.lo), "--"); snprintf(cur.hi, sizeof(cur.hi), "--"); }
        // Zero outdoor humidity is not a dry day, it is a field the hub did not send. (Wind is
        // left alone on purpose: 0 m/s is still air, a real thing, and feels already falls back
        // to the measured temperature.)
        const int oh = rtc_wx_hum;   // a percentage: outside 1..100 it is not a reading
        if (oh > 0 && oh <= 100) snprintf(cur.ohum, sizeof(cur.ohum), "%d", oh);
        else                snprintf(cur.ohum, sizeof(cur.ohum), "--");
        snprintf(cur.wind, sizeof(cur.wind), "%d", (int)lroundf(rtc_wx_wind)); // 3.9 m/s reads as noise
        snprintf(cur.feels,sizeof(cur.feels),"%d", (int)lroundf(Tdisp(rtc_wx_feels)));
    }

    // The configurable bottom strip. Formatted here, in the frame, like everything else the screen
    // draws. Each field keeps its dashes when the value was never fetched — a sentinel, not a zero,
    // because 0 % cloud is a real reading.
    {
        NVSManager &nv = NVSManager::getInstance();
        uint32_t packed = STRIP_DEFAULT; nv.getU32("strip", packed, STRIP_DEFAULT);
        cur.strip = packed;
        auto dash = [](char *dst, size_t cap) { snprintf(dst, cap, "--"); };
        // Clamped so the RANGE IS VISIBLE TO THE COMPILER, not to guard against real values: at -Os
        // gcc proves nothing about an int and rejects "%d" into eight bytes outright
        // (-Werror=format-truncation). The same knot was untied the same way on 2026-09-19 — make
        // the bound provable rather than switch the warning off, because next time it may be real.
        auto clampP = [](int v)   { return v < 0 ? 0 : (v > 999 ? 999 : v); };
        if (rtc_wx_uv >= 0.0f) snprintf(cur.uv, sizeof(cur.uv), "%d", clampP((int)lroundf(rtc_wx_uv)));
        else dash(cur.uv, sizeof(cur.uv));
        // Cloud and dew are still FETCHED and still reported by /api/status — they cost nothing,
        // the MET response carries them anyway, and they are how the owner tells "the hub does not
        // send this" from "the cell is broken". They are simply no longer offered as cells.

        // Sun and moon are worked out here, not fetched. That is the whole reason they are offered:
        // in Matter, in no_wifi and in Ultra Save on plain Wi-Fi there is no weather at all, and
        // these two stay true. They need a clock; the sun needs coordinates as well.
        cur.sunrise[0] = cur.sunset[0] = '\0';
        cur.moonQ = -1;
        if (wall > 1600000000) {
            cur.moonQ = (int8_t)((int)(moon_phase(wall) * 48.0) % 48);
            float la = 0.0f, lo = 0.0f;
            nv.getFloat("wx_lat", la, 0.0f); nv.getFloat("wx_lon", lo, 0.0f);
            if (la != 0.0f || lo != 0.0f) {
                const SunTimes st = sun_times(la, lo, wall, (int)lroundf(tz * 3600.0f));
                if (st.valid) {
                    snprintf(cur.sunrise, sizeof(cur.sunrise), "%02d:%02d", (st.rise_min/60) & 31, (st.rise_min%60) & 63);
                    snprintf(cur.sunset,  sizeof(cur.sunset),  "%02d:%02d", (st.set_min/60) & 31, (st.set_min%60) & 63);
                } else {
                    // Polar day or night. Dashes would read as a fault, so the strip says which it
                    // is: the sun is up all day, or it is not coming up at all.
                    snprintf(cur.sunrise, sizeof(cur.sunrise), st.always_up ? "up" : "--");
                    snprintf(cur.sunset,  sizeof(cur.sunset),  st.always_up ? "all" : "--");
                }
            }
        }
    }

    // The outlook row: tomorrow and the three days after it, matched by DATE against today rather
    // than taken in the order stored. Only in the layout with an outdoor block — the offline one
    // draws no row, and a field the renderer never draws must not make two otherwise identical
    // frames differ, or the "nothing changed" check below stops firing. No clock, no today to count
    // from, so no row.
    if (cur.noNet != 2 && wall > 1600000000) {
        const int32_t today = (int32_t)(((long long)wall + lroundf(tz * 3600.0f)) / 86400);
        static const char *WD[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };   // tm_wday order
        // RTC memory holds whatever the RAM did when power came back, and a date that happens to
        // match does not rule that out. A slot is believed only if it also looks like a forecast.
        auto plausible = [](const WxDay &f) {
            return f.lo > -90.0f && f.lo < 70.0f && f.hi > -90.0f && f.hi < 70.0f &&
                   isdigit((unsigned char)f.icon[0]) && isdigit((unsigned char)f.icon[1]);
        };
        int have = 0;
        for (int i = 0; i < 4; i++) {
            const int32_t want = today + 1 + i;
            // "Sat 12": the weekday people plan by, and the date that says which Saturday. Both come
            // from the slot's own day number, so the label cannot disagree with the day it heads.
            const time_t at = (time_t)want * 86400;
            struct tm dm; gmtime_r(&at, &dm);
            snprintf(cur.fc_day[i], sizeof(cur.fc_day[i]), "%s %d", WD[dm.tm_wday % 7], dm.tm_mday % 32);
            const WxDay *hit = nullptr;
            for (const WxDay &f : rtc_fc) if (f.day == want && plausible(f)) { hit = &f; break; }
            if (!hit) { snprintf(cur.fc_hi[i], sizeof(cur.fc_hi[i]), "--"); continue; }
            snprintf(cur.fc_icon[i], sizeof(cur.fc_icon[i]), "%.3s", hit->icon);
            // No plus sign. Beside a 28 px icon a two-digit high WITH its sign is wider than the
            // column at size 2, and the size-2 digits are the whole point of this layout. A minus
            // still prints: below zero is information, above it is the ordinary case.
            snprintf(cur.fc_hi[i], sizeof(cur.fc_hi[i]), "%d", (int)lroundf(Tdisp(hit->hi)));
            snprintf(cur.fc_lo[i], sizeof(cur.fc_lo[i]), "%d", (int)lroundf(Tdisp(hit->lo)));
            have++;
        }
        if (have) cur.fc_n = 4;
        else { memset(cur.fc_day, 0, sizeof(cur.fc_day)); memset(cur.fc_hi, 0, sizeof(cur.fc_hi)); }
    }

    // Every string the renderer touches must be a real string: it dereferences most of them
    // unconditionally, so a zeroed struct faulted on the first unset field.
    auto fill = [&](ScreenData &d, const Frame &f) {
        d = ScreenData{};
        d.presU = f.presU[0] ? f.presU : "hPa";
        d.presMode = f.presMode; d.ptrend = f.ptrend;
        d.noNet = f.noNet;
        d.tStart = "--:--"; d.tEnd = "--:--"; d.tMid = "--:--"; d.graphSpan = "24H";
        d.fresh   = f.fresh[0]   ? f.fresh   : "now";
        d.wxFresh = f.wxFresh[0] ? f.wxFresh : "now";
        d.setupLine = f.setup[0] ? f.setup : nullptr;   // from the frame, never from a live value
        d.setupLine2 = (f.setup[0] && f.setup2[0]) ? f.setup2 : nullptr;

        // The renderer wants floats; the ring stores ppm as integers to keep a record at 32 bytes.
        static float gbuf[48]; static char gs[8], ge[8];
        for (int i = 0; i < f.g_n && i < 48; i++) gbuf[i] = (float)f.g_co2[i];
        d.hist = (f.g_n > 1) ? gbuf : nullptr;
        d.histN = f.g_n;
        auto hhmm = [&](uint32_t ts, char *out) {
            const time_t lt = (time_t)ts + (time_t)f.tzmin * 60;   // the frame's own offset
            struct tm g; gmtime_r(&lt, &g);
            snprintf(out, 8, "%02d:%02d", g.tm_hour % 24, g.tm_min % 60);
        };
        // The tag has to describe what is actually plotted. 48 records five minutes apart is four
        // hours, not the twenty-four the label used to claim — and after a gap in the log the same
        // 48 points can span days. So it is measured from the samples, never assumed.
        static char gspan[8];
        static char gm[8];
        if (f.g_n > 1) {
            hhmm(f.g_t0, gs); hhmm(f.g_t1, ge); d.tStart = gs; d.tEnd = ge;
            hhmm(f.g_t0 + (f.g_t1 - f.g_t0) / 2, gm); d.tMid = gm;   // the offline graph labels it
            const uint32_t sec = (f.g_t1 > f.g_t0) ? (f.g_t1 - f.g_t0) : 0;
            if      (sec >= 172800) snprintf(gspan, sizeof(gspan), "%uD", (unsigned)(sec / 86400));
            else if (sec >= 3600)   snprintf(gspan, sizeof(gspan), "%uH", (unsigned)(sec / 3600));
            else                    snprintf(gspan, sizeof(gspan), "%uM", (unsigned)(sec / 60));
            d.graphSpan = gspan;
        }

        d.date = f.date[0] ? f.date : "--.--.----";
        d.time = f.time[0] ? f.time : "--:--";
        // CO2, temperature and humidity all come out of the SAME sensor, so when it is down they
        // are down together. rtc_co2 keeps the last good number for ever, which would have left a
        // confident hero reading above two blocks of dashes — the one number a passer-by actually
        // reads, and the one that would be wrong.
        d.co2  = (f.co2[0] && !f.err) ? f.co2 : "---";
        d.temp = f.temp[0] ? f.temp : "--";
        d.hum  = f.hum[0]  ? f.hum  : "--";
        d.pres = f.pres[0] ? f.pres : "--";
        d.cal  = f.cal[0]  ? f.cal  : nullptr;
        d.aq   = f.aq[0]   ? f.aq   : "--";
        d.aqInvert = f.aqInv; d.aqLevel = f.aqLevel; d.humDamp = f.humDamp;
        d.err  = f.err;
        // The air verdict leans on humidity for the damp class and reads the room's CO2 for the
        // stuffy one, so with the climate sensor down it is not merely unknown, it is unfounded.
        // Say so, in the one block on the screen that is meant to carry the alarm.
        if (d.err) { d.aq = "Error"; d.aqInvert = true; d.aqLevel = -1; d.cal = nullptr;
                     d.humDamp = false; }   // no humidity reading, nothing to call damp
        d.night = f.night; d.updClock = f.updClock;
        d.usb = f.usb; d.chg = f.chg; d.lowbat = f.lowbat; d.wifiFail = f.wifiFail;
        d.batt = f.batt; d.outT = f.outT;

        d.city = nullptr; d.desc = "--"; d.wxIcon = "01d";
        d.mmLo = "--"; d.mmHi = "--"; d.outHum = "--"; d.wind = "--"; d.feels = "--";
        if (f.city[0]) {
            d.city = f.city; d.desc = f.desc; d.wxIcon = f.icon;
            d.mmLo = f.lo; d.mmHi = f.hi; d.outHum = f.ohum; d.wind = f.wind; d.feels = f.feels;
        // The configurable bottom strip. Mapped straight across: the frame already holds finished
        // strings, and an empty sunrise means the device has no clock or no coordinates — the cell
        // then draws its own dashes rather than being handed a fabricated time.
        d.strip   = f.strip;
        d.uv      = f.uv[0]    ? f.uv    : "--";
        d.sunrise = f.sunrise[0] ? f.sunrise : nullptr;
        d.sunset  = f.sunset[0]  ? f.sunset  : nullptr;
        d.moon    = (f.moonQ >= 0) ? (float)f.moonQ / 48.0f : -1.0f;
        }
        d.fcN = f.fc_n;
        for (int i = 0; i < 4; i++) {
            d.fcDay[i] = f.fc_day[i]; d.fcIcon[i] = f.fc_icon[i];
            d.fcHi[i]  = f.fc_hi[i];  d.fcLo[i]   = f.fc_lo[i];
        }
    };

    // Nothing to say, nothing to draw. At night this is most wakes: the device still has to get up
    // every 300 s for the gas sensor, but with no clock ticking and CO2 read on a slower cadence,
    // the picture is usually identical to the one already on the glass. Repainting it would cost
    // energy and add a little more ghosting for no visible change.
    if (!full && rtc_prev_valid && memcmp(&cur, &rtc_prev, sizeof(Frame)) == 0) {
        ESP_LOGW(TAG, "screen unchanged — panel not touched");
        return 0;
    }


    const bool was_valid = rtc_prev_valid;
    ESP_LOGW(TAG, "drawing dashboard (%s) ...",
             !full && rtc_prev_valid ? "partial" : rtc_prev_valid ? "fast clean" : "full refresh");
    const int64_t t0 = esp_timer_get_time();
    ScreenData d;
    // BOTH mechanisms, on purpose, after removing this one for two hours cost the clock and the
    // freshness tag. refresh() now also writes the displayed image into 0x26 the way the library
    // does, and if that survives the sleep this repaint is redundant — but the evidence says it
    // does not survive, and redundant is the cheap side of that bet.
    //
    // What the owner saw, and it is a sharper reading than "residue accumulates between cleaning
    // frames": the partial does not ERASE. New digits land on top of old ones. A partial only
    // drives pixels where 0x26 and 0x24 disagree, so a pixel that is black on the glass while the
    // reference claims it is white never gets driven — the old ink simply stays, and the new is
    // added over it. That is 4.7's signature exactly, and it means the reference is not holding
    // the previous frame when the partial runs.
    //
    // So the previous frame is rendered and handed over again, as it was until this evening.
    // The cost is a second render per tick; the alternative is a screen that only adds.
    if (!full && rtc_prev_valid) {
        fill(d, rtc_prev);                // repaint what the glass already shows ...
        drawMainScreen(epd, d);
        epd.writeBaseline();              // ... and hand it over as the reference
    }
    const bool doing_full = full || !rtc_prev_valid;
    // A SCHEDULED cleaning frame is the vendor's fast refresh: 1.5 s and one flicker instead of a
    // full refresh's several, which is what makes cleaning often enough to matter bearable. The
    // thorough clear is kept for the case where we do not know what is on the glass — first boot,
    // a new image, a static screen that was drawn over the dashboard — because that is the one the
    // vendor says a full refresh is for.
    const EPaper::Update how = !doing_full        ? EPaper::Update::Partial
                             : !rtc_prev_valid    ? EPaper::Update::Full
                                                  : EPaper::Update::Fast;
    fill(d, cur);
    drawMainScreen(epd, d);
    epd.refresh(how);
    s_draw_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGW(TAG, "dashboard drawn in %lld ms", (long long)s_draw_ms);
    // Read before the sleep command: the reset that opens the next sequence clears it.
    const bool panel_ok = epd.sessionOk();
    // Sleep once the refresh is complete, as Good Display's demo does after every frame (see draw()).
    epd.hibernate();
    EvtLog::getInstance().setDraw((uint16_t)(s_draw_ms > 65535 ? 65535 : s_draw_ms), doing_full);
    // A frame that "finished" in a few hundred milliseconds did not happen: the transfer needs DMA
    // memory and without it goes silently nowhere (§4.19, measured at 323 ms against a healthy
    // 7358). Recording it makes the failure findable weeks later instead of only while watching.
    if (s_draw_ms < 150) EvtLog::getInstance().event(EVT_DRAW, (uint32_t)s_draw_ms);

    // Counted where the drawing actually happens. Doing it at the call site counted repaints that
    // were skipped as "unchanged", so the panel would have been fully refreshed on a schedule that
    // had nothing to do with how much ghosting had really accumulated.
    if (full || !rtc_prev_valid) { rtc_last_full = time(NULL); rtc_partials = 0; }
    else                           rtc_partials++;

    // memcpy, not assignment: the comparison above is a byte compare, and a member-wise copy
    // leaves the struct's padding untouched — stale padding would make two identical frames look
    // different and the check would never fire.
    memcpy(&rtc_prev, &cur, sizeof(Frame));
    // A frame that did not arrive whole (SPI error, BUSY timeout) is no reference for a partial: the
    // next one is a full refresh.
    rtc_prev_valid = panel_ok;
    if (!panel_ok) ESP_LOGE(TAG, "panel did not complete the frame — next frame is a full refresh");
    return (full || !was_valid) ? 2 : 1;
}

// A setup window or console opened from the working cycle has just closed, with the radio already
// down. Repaint in full to the settings now on file: `before` was read before the window opened, and
// changing the mode is one of the main reasons the window exists. A closing frame drawn to the
// settings the owner had just replaced — Ultra Save still showing a live clock and weather — is the
// clearest way to tell them the change did not take. Returns the plan the device now runs; `drew`
// gets draw_dashboard()'s result.
static Plan repaint_after_window(EPaper &epd, const Plan &before, const char *window, int &drew)
{
    global_portal_remain = -1;
    ESP_LOGW(TAG, "%s closed | free heap %u", window, (unsigned)esp_get_free_heap_size());
    const Plan after = readPlan(time(NULL));
    if (after.profile != before.profile || after.live_clock != before.live_clock)
        ESP_LOGW(TAG, "mode changed while the %s was open — redrawing as %d", window, after.profile);
    drew = draw_dashboard(epd, true, after.night, after.offline, after.live_clock);
    return after;
}

// Access point + captive DNS + web console. The captive DNS is what makes the setup page open
// by itself once a phone joins the network.
static void radio_up(void)
{
    // Use the project's own WiFiManager instead of driving esp_wifi by hand. It is the code the
    // old firmware used and it does the part that was missing here: registering the Wi-Fi and IP
    // event handlers. Without them a join never learns that it succeeded — the connect call sat
    // waiting for an event nobody delivered, so every attempt "failed" no matter the password.
    // startAP() also brings the interface up as APSTA, which is what lets the device scan and
    // later join while the phone stays on our own access point.
    ESP_LOGW(TAG, "starting AP \"%s\" (WiFiManager) ...", BENCH_AP_SSID);
    NVSManager::getInstance().init();          // Wi-Fi credentials live in NVS
    WiFiManager::prepareApPassword();          // before the driver loads (normally done at boot already)
    if (!WiFiManager::getInstance().init() || !WiFiManager::getInstance().startAP()) {
        // The QR screen is already on the glass promising a network that will not appear. Say so
        // in the log and stop here rather than standing up a web server and a captive DNS with
        // nothing to serve them on; the window then times out and the device goes back to sleep.
        ESP_LOGE(TAG, "access point did not come up — the setup window has no network");
        return;
    }

    WebServer::getInstance().start();
    CaptiveDNS::getInstance().start();
    ESP_LOGW(TAG, "AP + web console up at 192.168.4.1 | free heap %u",
             (unsigned)esp_get_free_heap_size());
}

// Everything down, in the reverse order. This must happen BEFORE the next refresh: sending 15 KB
// over SPI needs DMA-capable memory, and with Wi-Fi and httpd holding their buffers there may be
// none left — the transfer then silently does not happen at all (measured: 7358 ms with 316 KB
// free vs 323 ms and a blank screen with 226 KB free).
static void radio_down(void)
{
    ESP_LOGW(TAG, "stopping web console and AP ...");
    CaptiveDNS::getInstance().stop();
    WebServer::getInstance().stop();
    WiFiManager::getInstance().shutdown();   // disconnect, stop, deinit (wifi.rst)
    vTaskDelay(pdMS_TO_TICKS(200));      // let the drivers actually release their buffers
    ESP_LOGW(TAG, "radio down | free heap %u", (unsigned)esp_get_free_heap_size());
}

// The console asked for a factory reset. Record the request where a power cut cannot lose it, then
// restart into the boot that carries it out (factory_wipe). Nothing runs in between that could write a
// setting back, because the radio goes first. The panel gets its sleep command and its supply is
// latched ON across the restart: a pad hold survives a software reset, so the controller is not cut
// mid-state by its gate floating. That boot draws the logo over whatever the glass shows.
[[noreturn]] static void restart_into_factory_reset(EPaper &epd)
{
    led(true);
    ESP_LOGW(TAG, "factory reset requested — restarting into the wipe");
    vTaskDelay(pdMS_TO_TICKS(800));            // let the page's reply actually leave
    CaptiveDNS::getInstance().stop();
    WebServer::getInstance().stop();
    WiFiManager::getInstance().shutdown();
    NVSManager::getInstance().init();
    if (!NVSManager::getInstance().setU8("fr_pending", 1))
        ESP_LOGE(TAG, "could not record the request in NVS — RTC memory carries it across the restart");
    rtc_factory_req = FACTORY_REQ_MAGIC;
    panel_to_sleep(epd, PanelSleep::Powered);
    // A restart somebody asked for is not a boot that failed. Left counted, a reset asked for from
    // safe mode would land straight back in safe mode, which is checked before the wipe runs, and the
    // device could never be reset out of it.
    rtc_boot_incomplete = 0;
    esp_restart();
}

// Set when a window ended because the console asked for a restart; its caller then restarts instead of
// carrying on (restart_from_window). One per boot: the restart is the end of it.
static bool s_window_reboot = false;
static bool s_window_poweroff = false;   // the console asked to switch the device off
// Defined next to bench_screens_run(), where the rest of switching off lives; declared here
// because every path that closes a setup window has to be able to take it.
[[noreturn]] static void enter_power_off(EPaper &epd);

// The restart asked for from the console, carried out here: this task owns the panel, and the radio is
// already down. Not a boot that failed, so it is not counted toward the rollback.
[[noreturn]] static void restart_from_window(EPaper &epd)
{
    ESP_LOGW(TAG, "restarting, as asked from the console");
    panel_to_sleep(epd, PanelSleep::Powered);
    rtc_boot_incomplete = 0;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

// Hold the setup window open. Returns when it times out or the user ends it with a short press.
// The countdown restarts on every web request so filling in the wizard cannot time out.
// Returns whether anybody used it — a request from a page, or the button closing it — so a window a
// phantom opened with nobody there can be told from one a person came to.
// The device does NOT judge whether the air is outdoor air — the owner does, by starting this.
//
// It used to try, from the run-up's spread and mean, and that could not work: the test was applied
// to the very reading this calibration exists to correct. A sensor drifted +350 ppm reads 770 in
// genuinely fresh air, was told "that is a room", and could never satisfy the threshold again —
// while ASC, the only other way a baseline moves, is unavailable in single-shot mode. So the guard
// made permanent the error it was guarding against (owner's device, 2026-09-30: other sensors read
// ~500 in the same air, this one ~800).
//
// The asymmetry we had backwards on 2026-09-16: a calibration made in the wrong air is UNDONE by
// calibrating again in the right air. A calibration refused is not undone by anything. So the
// device asks once, before it starts, in words — and then believes the answer.
//
// The numbers are still read, still frozen, still reported. They stopped being a decision and
// became what they can honestly be: a description, and an estimate of the drift being corrected.
static constexpr int CAL_MEAN_ROOMY = 600;   // ppm: above this the report says so, afterwards

// ---- settling: waiting for the air INSIDE THE CASE to be the air outside ------------------------
// The datasheet asks for two things (SCD4x 3.8.1): "at least 3 minutes" AND "in an environment with
// a homogenous and CONSTANT CO2 concentration". The six readings satisfy the first twice over. The
// second is about WHEN YOU START, and until 2026-09-30 nothing enforced it: the owner carried the
// device out, pressed at once, and the first readings were of room air still leaving the enclosure.
// The run-up mean then sat above true outdoor air and the FRC over-corrected.
//
// The sensor itself is fast enough — tau63% is 60 s, so six minutes is six time constants — but the
// same datasheet line carries the footnote that matters: "Response time depends on DESIGN-IN". The
// 60 s is measured on Sensirion's bare evaluation kit; ours is behind a panel in a closed case.
//
// So the device waits until its readings STOP FALLING rather than for a fixed number of minutes,
// and that choice is not a preference: a level test would be poisoned by the very drift this
// calibration exists to remove (a sensor 350 ppm high reads fresh air as 770), while a test on the
// CHANGE is immune to it — the offset is constant and cancels in a difference.
static constexpr int      CAL_SETTLE_MIN   = 5;    // readings before settling may be declared
static constexpr int      CAL_SETTLE_MAX   = 20;   // ...and after this we proceed anyway, saying so
static constexpr int      CAL_SETTLE_DROP  = 15;   // ppm: fall across the last three that still counts
static constexpr uint32_t CAL_HOLD_MS      = 35u * 60u * 1000u;   // the window's ceiling may yield

static bool hold_setup_window(EPaper &epd, const char *addr)
{
    // Read by settle_pending_image() at the end of this wake: a wake that served a page is not a
    // wake whose free heap says anything about the image.
    s_served_page = true;

    // The CO2 sensor runs here as it does everywhere else — power-cycled single shots — once a
    // minute while the console is open. That gives the page a live reading, and it is the run-up
    // Sensirion prescribes before a forced recalibration in this mode: readings at a 1-minute
    // sampling period (datasheet §3.8.1, app note §5.1). Up to 0.9.43 it ran in periodic mode instead,
    // at 15–18 mA, and a restart straight out of this window (an update, the Restart button) left it
    // measuring continuously until the console was next opened and closed.
    I2CManager::getInstance().init();
    i2c_master_bus_handle_t cal_bus = I2CManager::getInstance().getBusHandle();
    static SCD41 cal_scd;
    const bool co2_ok = cal_bus && cal_scd.init(cal_bus);
    bool     shot_active  = false;               // converting; no command may interrupt it (§3.4)
    bool     shot_settled = false;               // that shot started with the offset on file
    uint32_t shot_at      = 0;
    uint32_t next_shot_at = esp_log_timestamp(); // the first reading straight away
    uint32_t next_bat_at  = esp_log_timestamp(); // and the cell, on its own much cheaper rhythm
    bool     cal_running  = false;               // a calibration request is taking its run-up
    uint32_t cal_since    = 0;                   // readings started from here count toward it
    int      cal_lo = 0, cal_hi = 0, cal_sum = 0;  // what the run-up has read so far, for the check
    bool     cal_settled = false;                 // the air in the case has stopped changing
    int      cal_settle_n = 0;                    // readings taken while waiting for that
    int      cal_trail[6] = {0};                  // the last readings, oldest first — for the glass
    int      cal_trail_n = 0;
    uint32_t cal_started_ms = 0;                  // when the owner pressed, for "running N min"
    int      cal_drawn = -1;                      // what the glass currently shows, so it is not
                                                  //   repainted with the picture already on it
    bool     cal_page_gone = false;              // the tab left while the run-up was still going
    // Every way out of this window goes through here. A shot still converting is waited out, then the
    // sensor goes back to sleep; a calibration that has not finished is reported as failed.
    auto co2_close = [&]() {
        if (co2_ok && shot_active) {
            const uint32_t since = esp_log_timestamp() - shot_at;
            if (since < SCD41::SINGLE_SHOT_MS) {
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(SCD41::SINGLE_SHOT_MS - since + 20) + 1);
            }
            cal_scd.powerDown();
            shot_active = false;
        }
        if (cal_running || global_calib_result == 2) {
            global_calib_error = 1; global_calib_req = 0; global_calib_result = -1;
            global_calib_confirm = 0;
            cal_running = false;
            ESP_LOGW(TAG, "setup window closed before the CO2 calibration finished");
        }
    };
    // Somebody opened the console. Which way in matters: on the device's own access point they were
    // standing next to it, on the home network they were not.
    EvtLog::getInstance().event(EVT_SEC, SEC_WINDOW_OPEN | (addr && addr[0] ? 0u : 0x100u));
    ESP_LOGW(TAG, "CO2 sensor for the setup window: %s",
             co2_ok ? "one single shot a minute" : "NOT AVAILABLE");

    uint32_t left_ms  = PORTAL_SEC * 1000u;
    uint32_t http_at  = global_last_http_ms;
    const uint32_t http_at_open = http_at;     // for "did anybody use this window"
    // The hard ceiling on one window, whatever happens inside it.
    //
    // Everything else here is a timeout that any deliberate request pushes back: three minutes, or
    // fifteen with keep-open. There was no upper limit at all, so whoever reached the window first
    // could hold it open for ever — a request every couple of minutes was enough, and an upload
    // refreshed it on every 2048-byte chunk. That turns "the console exists for three minutes after
    // somebody pressed the button" — which is the physical-presence argument the technical file
    // rests on (Docs/compliance_2026-09-18.md, ACM-1 / DT.ACM-1.DN-2) — into a claim that is simply
    // not true, and it keeps the radio up and the whole API exposed for as long as anyone likes.
    //
    // Forty minutes: the longest honest session is the first-run wizard read slowly with the screen
    // guide open, which is minutes, and an update from this window is capped by its own 30 s
    // extension plus the install. An install in flight still blocks the close below, so the ceiling
    // never cuts a firmware write in half.
    const int64_t window_deadline_us = esp_timer_get_time() + 40LL * 60 * 1000000;
    bool ceiling_noted = false;
    uint32_t closing_at = 0;      // set by the goodbye beacon; any request cancels it
    uint32_t next_log = left_ms;

    // The press that opened this window may still be down — the long press is only released after
    // five blinks, and a held button is not waited for indefinitely. Until it has been seen up
    // once, ignore it: otherwise the finger that asked for the window is counted as the short
    // press that closes it, and the window shuts the instant it opens.
    bool btn_seen_up = false;
    uint8_t btn_low_n = 0;        // consecutive low readings since the button was last seen up
    bool install_note = false;    // "stays open for the update" said once
    // A pressed install downloading, or done and about to restart (the install task restarts 2.5 s
    // after StDone): the window must not end under either.
    auto install_holds_window = []() {
        // The local upload counts too. It writes the same slot and it is the window's radio it is
        // arriving over, but it goes nowhere near OtaInstall::state — so a 3 MB push over a weak
        // link was cut off by the ordinary three-minute timeout, with nothing to say why.
        return OtaInstall::state == OtaInstall::StRunning || OtaInstall::state == OtaInstall::StDone
            || OtaInstall::local_upload;
    };
    // co2_close() can wait out a reading for up to five seconds while the web server still answers; a
    // restart asked for in that time was answered "ok" and must not be dropped.
    auto note_late_reboot = [&]() {
        if (global_reboot_req) {
            global_reboot_req = false;
            s_window_reboot = true;
            ESP_LOGW(TAG, "the console asked for a restart while the window was closing");
        }
        if (global_poweroff_req) {
            global_poweroff_req = false;
            s_window_poweroff = true;
            ESP_LOGW(TAG, "the console asked to switch off while the window was closing");
        }
    };

    // Left-open reminder. The console keeps the radio up, which is the most expensive thing this
    // device does, and the one way to leave it running is to tap "keep open" and then walk off —
    // the window survives a quarter of an hour that way. On the cable there is nothing to save, so
    // this only applies on battery, and it is said once.
    uint32_t open_ms = 0, idle_ms = 0;
    bool nagged = false;
    bool keep_was = global_portal_keep;
    const bool on_battery = !usb_present();
    bool ota_done_hold = false;

    // Telling the owner how the calibration ended. The run-up now outlives a closed tab — a balcony
    // is no place to stand holding a phone for six minutes — so this is the only way the result gets
    // out. Same shape as every other message (0.9.57): icon, device and event on the first line,
    // then what happened, then where and when.
    auto cal_tell = [&](const char *icon, const char *event, const char *what) {
        uint8_t wl_mode = 0, tg_en = 0;
        NVSManager::getInstance().getU8("wl_mode", wl_mode, 0);
        NVSManager::getInstance().getU8("tg_en", tg_en, 0);
        std::string tok, chat;
        NVSManager::getInstance().getString("tg_tok", tok, "");
        NVSManager::getInstance().getString("tg_chat", chat, "");
        if (wl_mode > 1 || !tg_en || tok.empty() || chat.empty()) return;
        // Our own access point has no way out to Telegram; only a joined home network does.
        if (!WiFiManager::getInstance().isConnected()) return;
        char nbuf[40], when[16], m[480];
        const char *dev = device_name(nbuf, sizeof(nbuf));
        local_stamp(time(NULL), when, sizeof(when));
        snprintf(m, sizeof(m), "%s %s \xC2\xB7 %s\n%s\n%s%s%s", icon, dev, event, what, dev,
                 when[0] ? " \xC2\xB7 measured " : "", when);
        ESP_LOGW(TAG, "notifying: %s", event);
        Telegram::send(tok, chat, m);
    };

    // Sensirion's forced recalibration (3.8.1). NEVER deferred: the sensor answers 0xFFFF if it was
    // not operated shortly before the command, so this is only ever called from inside the branch
    // that has just completed a reading — including when what released it was the owner answering
    // the question below, which is exactly why the readings carry on while that question is up.
    // The glass is how this reaches its owner. They are outside holding the device; the page may be
    // closed, the phone asleep, and Telegram may never have been set up — and since the settling
    // wait the run takes a quarter of an hour, not six minutes. Full refreshes at one a minute: the
    // same rate the dashboard already redraws at, and a partial would need a baseline this screen
    // does not own.
    auto cal_paint = [&](int phase, int value, int corr, const char *why) {
        if (cal_drawn == phase && phase < 2 && value < 0) return;
        CalibScreen c{};
        c.phase   = (uint8_t)phase;
        c.value   = (int16_t)value;
        c.corr    = (int16_t)corr;
        c.why     = why;
        c.done    = (uint8_t)((global_calib_samples > 6) ? 6 : global_calib_samples);
        c.trail_n = (uint8_t)cal_trail_n;
        for (int i = 0; i < cal_trail_n && i < 6; i++) c.trail[i] = (int16_t)cal_trail[i];
        if (cal_started_ms)
            c.ran_min = (uint16_t)((esp_log_timestamp() - cal_started_ms) / 60000u);
        if (!epd.init()) { ESP_LOGE(TAG, "no panel — the calibration runs unseen"); return; }
        drawCalibScreen(epd, c);
        epd.refresh(EPaper::Update::Full);
        cal_drawn = phase;
    };
    // Oldest first, six deep. This is what the owner reads to see the air settling, and afterwards
    // it is the only evidence of whether it had settled before the six readings were counted.
    auto cal_push = [&](int ppm) {
        if (cal_trail_n < 6) cal_trail[cal_trail_n++] = ppm;
        else { for (int i = 0; i < 5; i++) cal_trail[i] = cal_trail[i + 1]; cal_trail[5] = ppm; }
    };
    auto cal_do_frc = [&]() {
        const int ref = global_calib_req;
        int16_t corr = 0;
        ESP_LOGW(TAG, "forced recalibration to %d ppm after %d readings (%d..%d ppm, mean %d)",
                 ref, (int)global_calib_samples, (int)global_calib_lo, (int)global_calib_hi,
                 (int)global_calib_avg);
        const bool frc = cal_scd.performForcedRecalibration((uint16_t)ref, corr);
        global_calib_correction = corr;
        global_calib_error   = frc ? 0 : 2;
        global_calib_req     = 0;
        global_calib_result  = frc ? 1 : -1;
        global_calib_confirm = 0;
        cal_running = false;
        EvtLog::getInstance().event(EVT_SENSOR,
            frc ? (SENS_SCD_FRC_OK | (uint16_t)corr) : SENS_SCD_FRC_BAD);
        ESP_LOGW(TAG, "forced recalibration %s, correction %d ppm",
                 frc ? "done" : "REFUSED by the sensor", corr);
        char what[420];
        if (frc) {
            // The readings go out with the result, not as a verdict: they are the only way the owner
            // can tell a calibration that did something sensible from one made in the wrong air. And
            // a mean that looks like a room is said so AFTERWARDS, where it costs a repeat outside
            // instead of a device that can never be calibrated again.
            snprintf(what, sizeof(what),
                     "The CO2 baseline is anchored to fresh outdoor air, %d ppm. Correction %+d ppm.\n"
                     "It read %d-%d ppm, mean %d, over the six minutes before that.\n"
                     "%sYou can bring the device back to its place.",
                     ref, (int)corr, (int)global_calib_lo, (int)global_calib_hi, (int)global_calib_avg,
                     global_calib_unsettled
                       ? "The readings had not stopped falling when the six were taken, so the air "
                         "in the case may not have been the air outside. If the device reads low "
                         "now, run this again and give it longer outside.\n"
                       : global_calib_avg > CAL_MEAN_ROOMY
                       ? "That mean is high for outdoor air: either this sensor was reading that far "
                         "high - which is what was just corrected - or the device was not outside. "
                         "If it was not, run this again where it is.\n"
                       : "");
            cal_tell("\xE2\x9C\x85", "CO2 calibration done", what);
            cal_paint(2, ref, (int)corr, nullptr);
        } else {
            cal_tell("\xE2\x9A\xA0\xEF\xB8\x8F", "CO2 calibration failed",
                     "The sensor refused the calibration. Nothing was changed. Keep the device in "
                     "fresh outdoor air and start it again.");
            cal_paint(3, -1, 0, "nothing was changed - the sensor refused it");
        }
        // Nobody is watching the page, so there is nothing left to hold the radio up for.
        if (cal_page_gone && left_ms > 5000) left_ms = 5000;
    };

    while (left_ms > 0 || OtaInstall::state == OtaInstall::StRunning) {
        // The cell, kept live while the window is open.
        //
        // publish_globals() runs ONCE per cycle, before the window opens, so without this the page
        // shows a single snapshot for the whole session: pull the charger out and the number does
        // not move, which reads as a frozen device rather than a frozen field. Reported from the
        // bench on 2026-09-29, and it also makes the reading useless for comparing against a
        // multimeter — the two would be measuring different moments.
        //
        // Cheap enough to do often, unlike the rest: one ADC read on a v2 board or one I2C
        // transaction on a v1 one, against the five seconds a CO2 shot costs.
        //
        // VOLTAGE ONLY, deliberately. The percentage carries the charging ceiling that
        // BatteryMonitor applies (2026-09-21: a charger holding the terminals at 4.2 V is not a
        // full cell), and a raw number written here would undo it. The slope bookkeeping is left
        // alone for the same reason — its anchor spans hours, and a five-second refresh has no
        // business touching it.
        {
            const uint32_t now_ms = esp_log_timestamp();
            if (now_ms >= next_bat_at) {
                next_bat_at = now_ms + 5000;
                I2CManager::getInstance().init();
                float v = 0.0f;
                if (BatterySource::readVoltage(I2CManager::getInstance().getBusHandle(), v) && v > 2.0f) {
                    // Logged on change rather than on every read: silent while the cell sits
                    // still, loud the moment the charger goes in or out. That makes the cable a
                    // second way to read this number — the console needs a password, and pairing
                    // a reading with a multimeter should not depend on copying digits by eye.
                    if (fabsf(v - rtc_volt) > 0.005f)
                        ESP_LOGW(TAG, "cell %.3f V (USB %s)", v, usb_present() ? "in" : "out");
                    rtc_volt = v;
                    global_voltage = v;
                }
            }
        }
        // The window may not close under a running install. The transfer is 1.7 MB over TLS in its
        // own task, and the page watching it polls /api/status — which deliberately does NOT count
        // as activity, so nothing was refreshing the timer while the owner sat watching a bar. The
        // window then expired mid-download, the cycle went to sleep, and the task died with it:
        // the update simply never arrived, and nothing said why. Found the first time it was used.
        if (OtaInstall::state == OtaInstall::StRunning && left_ms < 30000) left_ms = 30000;

        // A queued security notice goes out here: the radio is up for the console anyway, and this
        // is the cycle task, so Telegram gets a real stack. Seconds after the owner pressed the
        // button, rather than at the next radio window half an hour later.
        if (WiFiManager::getInstance().isConnected()) deliver_security_note();

        // Past the ceiling the window stops being refreshable: the remaining time runs down and
        // nothing below puts it back. An install still holds it open — that check is the one thing
        // allowed to outlive this, because a half-written slot is worse than a long window.
        // A calibration in flight holds the ceiling back too, and for the same reason an install
        // does: the owner is standing outside and cutting it at minute 40 wastes the trip. Bounded,
        // not open-ended — the run has its own cap (CAL_SETTLE_MAX + six readings), and this is that
        // cap plus slack, measured from the press.
        const bool cal_holds = (global_calib_result == 2) && cal_started_ms &&
                               (esp_log_timestamp() - cal_started_ms) < CAL_HOLD_MS;
        if (esp_timer_get_time() > window_deadline_us && !install_holds_window() && !cal_holds) {
            if (!ceiling_noted) {
                ceiling_noted = true;
                ESP_LOGW(TAG, "window open 40 min — closing, whatever the page is doing");
            }
            if (left_ms > 3000) left_ms = 3000;
        }
        // Installed, restart pending (the install task restarts 2.5 s after StDone). Closing now would
        // deep-sleep with otadata already switched — what BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP's
        // Kconfig help warns against. Once, for a few seconds.
        if (OtaInstall::state == OtaInstall::StDone && !ota_done_hold) {
            ota_done_hold = true;
            if (left_ms < 10000) left_ms = 10000;
        }
        // Nor under a CO2 calibration: its run-up takes about six minutes and the page only polls.
        if (global_calib_result == 2 && left_ms < 90000) left_ms = 90000;
        // Stopped from the page while the run-up was still going. This is what replaced the "are you
        // sure?" box in front of the button: a mis-tap is undone by a Stop that stays available for
        // the whole six minutes, rather than by a question asked before anything has happened. The
        // sensor has been read and nothing else, so there is nothing to undo.
        if (global_calib_result == 2 && global_calib_confirm == -1) {
            ESP_LOGW(TAG, "CO2 calibration stopped from the page after %d readings",
                     (int)global_calib_samples);
            global_calib_error = 4; global_calib_req = 0; global_calib_result = -1;
            global_calib_confirm = 0;
            cal_running = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        if (left_ms >= 50) left_ms -= 50; else left_ms = 0;
        open_ms += 50;
        idle_ms += 50;

        // Only DELIBERATE actions extend the window — saving, scanning, running the wizard. The
        // page's own status polling is deliberately not counted (see web_server.cpp): counting it
        // made the window immortal while a tab sat open, and made the countdown lie.
        esp_task_wdt_reset();          // the setup window can legitimately last a quarter of an hour

        // A factory reset from the console. Checked here because this loop is the only place the web
        // server ever runs, so every console that can send the request is one this loop is serving.
        if (global_factory_req) {
            global_factory_req = false;
            co2_close();
            restart_into_factory_reset(epd);
        }
        // A restart from the console: the window ends here and its caller restarts, radio down first.
        if (global_reboot_req) {
            global_reboot_req = false;
            co2_close();
            s_window_reboot = true;
            ESP_LOGW(TAG, "the console asked for a restart — closing the window");
            return true;
        }
        // "Close the console" from the page. Ends the window exactly the way the idle timeout ends
        // it — no restart, no panel work of its own: the caller owns the panel and the radio and
        // takes them down in order, and the closing repaint it does is the one the owner should
        // see. The alternative for somebody who is finished is to wait out three minutes with the
        // radio up, which is the most expensive thing this device does.
        if (global_close_req) {
            global_close_req = false;
            co2_close();
            note_late_reboot();
            ESP_LOGW(TAG, "the console asked to close — ending the window");
            return true;
        }
        // Switching off. Handled here beside the restart because this loop is the only place the
        // web server ever runs — and handled by CLOSING rather than by acting, for the same reason
        // the restart is: the caller owns the panel and the radio, and it takes them down in order.
        if (global_poweroff_req) {
            global_poweroff_req = false;
            co2_close();
            s_window_poweroff = true;
            ESP_LOGW(TAG, "the console asked to switch off — closing the window");
            return true;
        }

        // Settings that change the picture were just applied. Repaint now rather than at the next
        // wake: the user pressed Apply and is looking at the device. The plan is re-read first —
        // the mode they chose is exactly what changed — and the refresh is a full one, because a
        // new mode can rearrange the whole layout and a partial update has no way to erase what it
        // no longer draws. The address band stays: the console is still up, so it is still true.
        // Only when the glass shows the dashboard. With the device's own access point it shows the QR
        // code and the password that got the phone here, and a phone that drops off the network needs
        // them again; that window's closing repaint (or the next boot) brings the new settings in.
        if (global_screen_dirty && !addr) {
            global_screen_dirty = false;
            ESP_LOGW(TAG, "settings applied — the setup screen stays until the window closes");
        }
        if (global_screen_dirty) {
            global_screen_dirty = false;
            const Plan np = readPlan(time(NULL));
            ESP_LOGW(TAG, "settings applied — repainting: profile %d, sensors every %u min, "
                          "radio %s", np.profile, (unsigned)np.meas_min,
                     np.radio_min ? "on" : "off");
            draw_dashboard(epd, true, np.night, np.offline, np.live_clock, addr);
        }

        // "The page is going away" is not the same as "the person is going away", and the browser
        // cannot tell us which. A reload — F5, or a pull-to-refresh, which is one careless thumb on
        // a phone — fires the same event as closing the tab, and the window closed under somebody
        // who was still standing there, leaving them to walk to the device and hold the button. So
        // the beacon starts a short countdown instead of ending it outright: a page coming back
        // polls within a second or so, and any request at all cancels it. Nothing is lost when the
        // tab really did close — five seconds of radio against the three minutes this saves.
        if (global_portal_close) {
            global_portal_close = false;
            // A run-up outlives the page. Six minutes is longer than anybody wants to stand outside
            // holding a phone, so the readings carry on with the tab closed and the result goes out
            // over Telegram. The QUESTION is the opposite case: only the page can answer it, so a
            // page that left has answered no — noted here, acted on at the top of the loop.
            if (global_calib_result == 2) {
                cal_page_gone = true;
                ESP_LOGW(TAG, "page said goodbye — the CO2 calibration keeps running");
            } else {
                // Fifteen, not five. A reload on this device is not a quick round trip: the console
                // is served uncached, and the page has to arrive and run before it can say it is
                // back. Five seconds expired mid-download and took the window with it.
                closing_at = esp_log_timestamp() + 15000;
                ESP_LOGW(TAG, "page said goodbye — closing in 15 s unless it comes back");
            }
        }
        // Not while an install is downloading or about to restart: the window is what keeps its radio up.
        if (closing_at && esp_log_timestamp() >= closing_at && !install_holds_window()) {
            ESP_LOGW(TAG, "page gone — ending the setup window");
            co2_close();
            note_late_reboot();
            return true;                               // a page was there to say goodbye
        }

        // Keep-open is watched in its own right, not only as a side effect of a request arriving.
        // Tying the two together made the window's length depend on the order of two writes inside
        // a request handler — an ordering that is invisible from here and was wrong.
        if (global_portal_keep != keep_was) {
            keep_was = global_portal_keep;
            // Not past the ceiling: there the remaining time only runs down.
            if (!ceiling_noted) { left_ms = (keep_was ? 900u : (uint32_t)PORTAL_SEC) * 1000u; }
            next_log = left_ms;
            ESP_LOGW(TAG, "keep-open %s — window is now %u s",
                     keep_was ? "ON" : "off", (unsigned)(left_ms / 1000));
        }

        if (global_last_http_ms != http_at) {
            http_at  = global_last_http_ms;
            idle_ms  = 0;
            // Somebody is there after all — a reloaded page announcing itself, most likely.
            if (closing_at) { closing_at = 0; ESP_LOGW(TAG, "the page came back — staying open"); }
            cal_page_gone = false;
            // Settings change while this window is open — that is the whole point of it. The plan
            // is otherwise computed once, at the start of the wake, so applying Performance and
            // then reading "Optimal" back was the card faithfully reporting a stale answer. Same
            // defect as the closing repaint in 4.75, in a different reader. readPlan touches no
            // RTC state, so recomputing it here is free of consequence.
            publish_plan(readPlan(time(NULL)));
            if (!ceiling_noted) { left_ms = (global_portal_keep ? 900u : (uint32_t)PORTAL_SEC) * 1000u; }
            next_log = left_ms;
            ESP_LOGI(TAG, "user action — window restarted at %u s", (unsigned)(left_ms / 1000));
        }

        // Ten minutes open with nobody touching it for the last three: whoever opened this has
        // gone. Both halves matter — somebody working through a long form is not to be nagged, and
        // an idle minute on its own means nothing. The radio is already up, so saying so is free.
        if (on_battery && !nagged && open_ms >= 600000u && idle_ms >= 180000u) {
            nagged = true;
            uint8_t wl_mode = 0, tg_en = 0;
            NVSManager::getInstance().getU8("wl_mode", wl_mode, 0);
            NVSManager::getInstance().getU8("tg_en", tg_en, 0);
            std::string tok, chat;
            NVSManager::getInstance().getString("tg_tok", tok, "");
            NVSManager::getInstance().getString("tg_chat", chat, "");
            if (wl_mode <= 1 && tg_en && !tok.empty() && !chat.empty()) {
                char nbuf[40]; char m[300], since[16];
                local_stamp(time(NULL) - (time_t)(open_ms / 1000), since, sizeof(since));
                snprintf(m, sizeof(m),
                         "\xF0\x9F\x93\xB6 %s \xC2\xB7 Setup page still open\n"
                         "Wi-Fi has been on for %u minutes with nobody using the page.\n"
                         "Close the page or press the button on the device to save the battery.\n"
                         "Battery %.0f %%%s%s",
                         device_name(nbuf, sizeof(nbuf)), (unsigned)(open_ms / 60000), rtc_soc,
                         since[0] ? " \xC2\xB7 open since " : "", since);
                ESP_LOGW(TAG, "notifying: setup left open");
                Telegram::send(tok, chat, m);
            } else {
                ESP_LOGW(TAG, "setup left open %u min on battery (no Telegram to say so)",
                         (unsigned)(open_ms / 60000));
            }
        }
        global_portal_remain = (int)(left_ms / 1000);

        if (co2_ok) {
            const uint32_t now_ms = esp_log_timestamp();
            // A calibration asked for by the page. The handler cannot run it — the sensor belongs to
            // this loop — so it leaves a request, and the run-up starts now rather than at the next
            // minute.
            if (global_calib_result == 2 && !cal_running) {
                cal_running = true;
                cal_since = now_ms;
                cal_started_ms = now_ms;
                cal_settled = false; cal_settle_n = 0; cal_trail_n = 0; cal_drawn = -1;
                global_calib_settled = 0;
                global_calib_samples = 0;
                cal_lo = cal_hi = cal_sum = 0;
                // One statement each: chaining them reads the volatile back, which C++20 deprecates.
                global_calib_lo = 0;
                global_calib_hi = 0;
                global_calib_avg = 0;
                global_calib_confirm = 0;
                if (!shot_active) next_shot_at = now_ms;
                ESP_LOGW(TAG, "CO2 calibration to %d ppm: %d readings a minute apart, then FRC",
                         (int)global_calib_req, SCD41::FRC_RUNUP_SHOTS);
            }
            if (!shot_active && now_ms >= next_shot_at) {
                next_shot_at = now_ms + SCD41::FRC_RUNUP_PERIOD_MS;
                if (cal_scd.wakeToIdle()) {
                    // A temperature calibration made on this page reaches the sensor before the next
                    // reading, so the page can show its effect within the minute.
                    scd_sync_offset(cal_scd);
                    shot_settled = scd_offset_settled();
                    // Also the compensation value FRC asks for beforehand (§3.8.1).
                    // An unconfigured device has never taken a gas tick, so it has no pressure of
                    // its own to give — and whatever sits in that field is not its own either.
                    if (rtc_readings_are_ours() && rtc_pres_pa > 0.0f)
                        cal_scd.setAmbientPressure(rtc_pres_pa);
                    shot_active = cal_scd.measureSingleShot();
                    shot_at = now_ms;
                }
                if (!shot_active) {
                    cal_scd.powerDown();
                    if (cal_running) {          // the 1-minute cadence is broken: start over
                        global_calib_samples = 0;
                        cal_lo = cal_hi = cal_sum = 0;
                    }
                }
            } else if (shot_active && now_ms - shot_at >= SCD41::SINGLE_SHOT_MS) {
                bool rdy = false;
                const bool asked = cal_scd.isDataReady(rdy);
                // Ready is due at 5000 ms; one more second of polling before it counts as a miss.
                if ((asked && rdy) || now_ms - shot_at >= SCD41::SINGLE_SHOT_MS + 1000) {
                    shot_active = false;
                    uint16_t c = 0; float t = 0, h = 0;
                    const bool valid = asked && rdy && cal_scd.readMeasurement(c, t, h) && c > 0;
                    if (valid) {
                        rtc_co2 = c; global_co2 = c;
                        // Temperature and humidity too, but only from a shot taken with the offset now
                        // on file: then the page shows the calibration it was just given, the closing
                        // repaint shows it on the glass, and the next calibration measures against it.
                        // A shot that started before a new offset arrived is left out.
                        if (shot_settled && scd_offset_settled()) {
                            rtc_temp = t; rtc_hum = h;
                            global_s_temp = t; global_s_hum = h;
                            global_temp_seq = global_temp_seq + 1;
                        }
                    }
                    // Only readings started after the request count: the device has to be in the
                    // reference air for the whole run-up.
                    if (cal_running && shot_at >= cal_since) {
                        if (!valid) {
                            global_calib_samples = 0;         // the 1-minute cadence is broken
                            cal_lo = cal_hi = cal_sum = 0;
                        } else if (!cal_settled) {
                            // Step one: wait for the enclosure. The test is on the CHANGE, never on
                            // the level — see CAL_SETTLE_* above for why that distinction is the
                            // whole point. Settled when the last three have stopped falling.
                            cal_push((int)c);
                            cal_settle_n++;
                            bool flat = false;
                            if (cal_trail_n >= 3) {
                                const int a = cal_trail[cal_trail_n - 3], z = cal_trail[cal_trail_n - 1];
                                flat = (a - z) <= CAL_SETTLE_DROP;
                            }
                            if (cal_settle_n >= CAL_SETTLE_MIN && flat) {
                                cal_settled = true;
                                ESP_LOGW(TAG, "air settled after %d readings — starting the run-up",
                                         cal_settle_n);
                            } else if (cal_settle_n >= CAL_SETTLE_MAX) {
                                cal_settled = true;
                                global_calib_unsettled = 1;   // said in the result, not a refusal
                                ESP_LOGW(TAG, "air still moving after %d readings — running up anyway",
                                         cal_settle_n);
                            }
                            if (cal_settled) { cal_trail_n = 0; global_calib_samples = 0;
                                               cal_lo = cal_hi = cal_sum = 0;
                                               global_calib_settled = 1; }
                            cal_paint(cal_settled ? 1 : 0, (int)c, 0, nullptr);
                        } else if (global_calib_samples < SCD41::FRC_RUNUP_SHOTS) {
                            if (global_calib_samples == 0) { cal_lo = cal_hi = (int)c; cal_sum = 0; }
                            if ((int)c < cal_lo) cal_lo = (int)c;
                            if ((int)c > cal_hi) cal_hi = (int)c;
                            cal_sum += (int)c;
                            cal_push((int)c);
                            global_calib_samples = global_calib_samples + 1;   // volatile: no ++
                            cal_paint(1, (int)c, 0, nullptr);
                        }
                        if (cal_settled && global_calib_samples >= SCD41::FRC_RUNUP_SHOTS) {
                            global_calib_lo  = cal_lo;
                            global_calib_hi  = cal_hi;
                            global_calib_avg = cal_sum / SCD41::FRC_RUNUP_SHOTS;
                            cal_do_frc();
                        }
                    }
                    if (!valid) cal_scd.reinit();         // Sensirion's recovery (§3.10.5)
                    cal_scd.powerDown();
                }
            }
        } else if (global_calib_result == 2) {
            global_calib_error = 3; global_calib_req = 0; global_calib_result = -1;
        }

        if (left_ms <= next_log) {                 // heartbeat every 15 s
            ESP_LOGW(TAG, "setup window: %u s left", (unsigned)(left_ms / 1000));
            next_log = (left_ms > 15000u) ? left_ms - 15000u : 0;
        }

        // The console asked us to join the network the user just typed. Nothing else was doing
        // this, so credentials were being stored while no connection was ever attempted — the
        // device then called itself configured on the strength of a string alone.
        // connectSTAKeepAP joins WITHOUT dropping the AP, so the phone stays on this page.
        // On the home network the page itself travels over the station link: joining another network
        // now would cut it off, and a network that then failed would leave no way back in (there is no
        // access point in this window). The name is saved; the next connection uses it.
        if (global_wifi_connect_req > 0 && addr) {
            global_wifi_connect_req = 0;
            std::string js, jp;
            NVSManager::getInstance().getString("ssid", js, "");
            NVSManager::getInstance().getString("pass", jp, "");
            // "Joined" only for the very credentials this link runs on. A new password for the same
            // network has not been tried, and reporting it as joined hid a typo until the next window.
            wifi_config_t cur = {};
            const bool same = WiFiManager::getInstance().isConnected() &&
                              esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK &&
                              strncmp((const char *)cur.sta.ssid, js.c_str(), sizeof(cur.sta.ssid)) == 0 &&
                              strncmp((const char *)cur.sta.password, jp.c_str(), sizeof(cur.sta.password)) == 0;
            global_wifi_connect_state = same ? 2 : 3;
            ESP_LOGW(TAG, "%s", same ? "network saved — it is the one this page is on"
                                     : "network saved — used from the next connection, not under this page");
            left_ms = PORTAL_SEC * 1000u;
        }
        else if (global_wifi_connect_req > 0) {
            global_wifi_connect_req = 0;
            global_wifi_connect_state = 1;
            std::string js, jp;
            NVSManager::getInstance().getString("ssid", js, "");
            NVSManager::getInstance().getString("pass", jp, "");
            ESP_LOGW(TAG, "joining \"%s\" (AP stays up) ...", js.c_str());
            const bool ok = WiFiManager::getInstance().connectSTAKeepAP(js, jp);
            global_wifi_connect_state = ok ? 2 : -1;
            // Promote on success, and never demote here. A typed name that has never joined is
            // already unverified — /api/save clears the flag the moment the network CHANGES — so
            // there is nothing left for a failure to prove. Demoting on failure meant one missed
            // 12-second window declared a months-old installation unconfigured, and an
            // unconfigured device erases its measurement log on the next wake.
            if (ok) {
                NVSManager::getInstance().setU8("wifi_ok", 1);
                // Set once, on the first network this device ever joined, and cleared only by a
                // factory reset. This — not "is it configured right now" — is what decides whether
                // there is a past worth keeping.
                uint8_t prov = 0;
                NVSManager::getInstance().getU8("provisioned", prov, 0);
                if (!prov) {
                    NVSManager::getInstance().setU8("provisioned", 1);
                    ESP_LOGW(TAG, "first successful setup — this device now has a history to protect");
                }
            }
            ESP_LOGW(TAG, "join %s", ok ? "SUCCEEDED" : "FAILED — the saved network stays as it was");
            left_ms = PORTAL_SEC * 1000u;      // give the user time to react either way
        }

        // Two readings in a row, 50 ms apart, as Espressif's button component debounces (esp-iot-solution
        // iot_button: a level must hold for its debounce ticks). One low sample was enough before, and
        // any glitch on the line closed the console.
        const bool btn_low = gpio_get_level(PIN_USER_BTN) == 0;
        if (!btn_low) { btn_seen_up = true; btn_low_n = 0; }
        else if (btn_seen_up && btn_low_n < 2) btn_low_n++;
        if (btn_low_n >= 2 && install_holds_window()) {
            if (!install_note) {
                install_note = true;
                ESP_LOGW(TAG, "button pressed during an update — the window stays open until it is done");
            }
        } else if (btn_low_n >= 2) {   // a press = "I'm done"
            ESP_LOGW(TAG, "button pressed — closing the setup window early");
            wait_button_release("closing the window");   // the light simply goes out on the way to sleep
            // The press that closed the window also set the interrupt's flag on its way down, and
            // that flag means "somebody pressed while we were busy — wake straight back up for it".
            // So closing the console by hand queued a second, immediate cycle: an unscheduled CO2
            // reading, a full refresh, and on this owner's hub-mode device another Wi-Fi session,
            // for a press that had already been served. Settle and drop it — it belongs to the
            // press we just finished, not to a new one. Same treatment as the long-press path.
            vTaskDelay(pdMS_TO_TICKS(60));
            s_btn_pressed_while_awake = false;
            co2_close();
            note_late_reboot();
            return true;                               // a finger closed it: somebody was there
        }
    }
    co2_close();
    note_late_reboot();
    ESP_LOGW(TAG, "setup window timed out after %d s of no activity", PORTAL_SEC);
    return global_last_http_ms != http_at_open;
}

// What to do when this image cannot finish a boot. Two levels, because the remedy might itself be
// the thing that fails.
//
// The picture is the setup screen: it tells the owner to join the device's own access point, which
// is exactly what is on offer here. The AP, not the home network — a join depends on a router
// being there and is a plausible place to be crashing, and this path must not need anything
// outside the room. No I2C, no sensors, no measurement: those are the most likely culprits and
// none of them is needed to be recoverable.
[[noreturn]] static void safe_mode(EPaper &epd, bool with_console)
{
    ESP_LOGE(TAG, "SAFE MODE (%s) — the normal cycle is not completing",
             with_console ? "with console" : "picture only, radio left down");
    // The panel lines may still be latched from the last sleep, and a held pad ignores any
    // reconfiguration (driver/gpio.h). The ordinary cycle releases them much later than this.
    EPaper::releaseDeepSleepHold(true);
    WiFiManager::prepareApPassword();   // no radio yet in this boot (see the header)
    if (epd.init()) draw_setup_screen(epd, "SAFE MODE", nullptr);   // no I2C here, by design

    if (with_console) {
        radio_up();
        hold_setup_window(epd, nullptr);
        radio_down();
        global_portal_remain = -1;
        // Asked for by somebody at the console: the ordinary cycle gets a fresh chance.
        if (s_window_poweroff) enter_power_off(epd);   // never returns
        if (s_window_reboot) restart_from_window(epd);
    }

    panel_to_sleep(epd);
    arm_wake_pins(UsbWake::None);
    // Cleared because THIS boot completed. If the ordinary cycle is still broken it will spend
    // five more boots — about half a minute of awake time against an hour asleep — and come back
    // here. That is a duty cycle the battery can carry indefinitely, and it self-heals the moment
    // the cause was transient, which latching would not.
    ESP_LOGE(TAG, "SAFE MODE — sleeping 1 h (or until the button)");
    enter_deep_sleep(3600ULL * 1000000ULL, true);
}

// Decide whether the image now running deserves to stay. Called at the one moment that carries any
// information — the end of a wake that got all the way to sleep — and only while the bootloader is
// still holding this image on probation.
//
// The test is the cycle itself. A separate checklist would prove that a checklist passes; what
// matters is whether the device measured, drew and got here, which is what these numbers are.
//
// Committing happens on the FIRST wake, deliberately, and this is the sharpest thing the OTA design
// turned up: a wake out of deep sleep takes the bootloader's fast path and never reads otadata, so
// a pending image is never actually rolled back by sleeping — it stays pending for weeks and then
// reverts at the first unrelated brownout, long after anyone could connect the two. Anything that
// "lets the image settle for a few days" is therefore not caution, it is a delayed surprise.
static void settle_pending_image(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;      // nothing on probation: normal boot

    const uint32_t heap = esp_get_free_heap_size();
    // A round trip through NVS, because reading is not the half that matters. Settings, Wi-Fi
    // credentials and the gas calibration all have to be WRITTEN here after an update, and a store
    // that mounts read-only would pass any read-only probe and then quietly lose everything.
    //
    // It used to read the key "interval" — described in this comment as one "this firmware always
    // writes". Nothing writes it: it is read with a default of 300 and never stored. So the probe
    // returned false on every device that ever existed, and the self-test rejected EVERY update
    // with "nvs", permanently and silently, blacklisting each version as it arrived. Found on the
    // first real update, by seq 6 being refused on hardware. This is exactly the failure 4.53
    // warned about and believed it had fixed — it corrected the sensor and draw probes and left
    // this one, because a read that returns false looks like a legitimate answer.
    NVSManager &nv = NVSManager::getInstance();
    uint32_t probe = 0;
    const uint32_t token = 0xA5000000u | (uint32_t)(esp_log_timestamp() & 0xFFFFFFu);
    const bool nvs_ok = nv.init() &&
                        nv.setU32("ota_probe", token) &&
                        nv.getU32("ota_probe", probe, 0) &&
                        probe == token;

    // EVERY TEST HERE PASSES WHEN THE ANSWER IS UNKNOWN, and that bias is the whole design. The
    // failed-boot counter is the real safety net: it reverts a broken image within five boots no
    // matter what this function concludes. So a wrong "commit" costs five boots, while a wrong
    // "revert" is permanent and silent — the version lands in ota_bad and is never offered again.
    // The first draft got this backwards and would have reverted every single update; both of the
    // reads below only exist on a wake that measured and drew, and the first wake after an install
    // is whichever wake happens to come next.

    // Two of three, not three. This very board has a dead pressure channel in its BME690 (4.17) and
    // a CO2 sensor that has already needed a factory reset (4.23). A strict test would roll back
    // perfectly good firmware over hardware faults it cannot fix, for ever. -1 means this wake
    // never opened the bus at all — a clock-only tick — which is not evidence of anything.
    const bool bus_ok = (s_sensors_ok < 0) || (s_sensors_ok >= 2);

    // A refresh that "finished" far too quickly did not happen: the transfer needs DMA-capable
    // memory and silently does nothing without it (4.19). The threshold has to be read against the
    // right draw — s_draw_ms is written by draw_dashboard() alone, so it is always a 1-bit frame,
    // measured at ~500 ms for a partial and 1721 ms for a full one (4.38). 4.19's numbers (323 ms
    // broken against 7358 healthy) are 4-gray and belong to a path that never lands here; taking
    // the threshold from them is what made this reject healthy hardware. No draw at all is not a
    // failure either — a clock-only tick legitimately skips it, and so does the frozen-ink guard
    // in a cold room (4.30).
    const bool draw_ok = (s_draw_ms < 0) || (s_draw_ms > 150);

    // 120 KB is a Wi-Fi-mode number: that wake still has a radio window and a TLS handshake ahead of it.
    // With the Matter stack running the free heap is about 103 KB by design (measured 2026-09-17), and
    // no TLS ever happens in that boot — the console session that updates is a separate boot without
    // the stack. There is no measured floor for Matter mode, so, as above, unknown passes; without
    // this every update installed from a Matter console session was rejected at its first tick.
    //
    // A wake that served a page is excluded for the same reason. The console costs about 80 KB —
    // measured on the device 2026-09-17, 90.8 KB free with it open against a median of 172 KB on
    // ordinary ticks — and after a long press the Wi-Fi is deliberately left up through the sleep.
    // So the one number this test reads would be below the floor for a reason that has nothing to
    // do with the image, and a healthy update would be reverted and blacklisted for ever.
    const bool heap_ok = MatterMode::running() || s_served_page || (heap > 120u * 1024u);

    const char *why = nullptr;
    if      (!nvs_ok)  why = "nvs";
    else if (!bus_ok)  why = "i2c";
    else if (!draw_ok) why = "panel";
    else if (!heap_ok) why = "heap";

    if (!why) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGW(TAG, "new image committed (sensors %u/3, draw %lld ms, heap %u)",
                     (unsigned)s_sensors_ok, (long long)s_draw_ms, (unsigned)heap);
            // Tell the owner, but only about an update that actually took: this line is reached once,
            // on the first wake that carried the new image all the way to sleep. The queue itself is
            // RAM, so the flag has to outlive the restart the installer caused — hence RTC, and hence
            // here rather than in the installer.
            rtc_sec_updated = 1;
        } else {
            ESP_LOGE(TAG, "could not commit the new image — it will revert");
        }
        return;
    }

    // Say why, in flash, before going anywhere: the reboot is the last thing this image does, and
    // the previous one has no other way to learn what happened here. ota_bad stops the same version
    // being fetched and installed again tomorrow, and the day after, for ever.
    ESP_LOGE(TAG, "new image FAILED its first cycle (%s): sensors %u/3, draw %lld ms, heap %u",
             why, (unsigned)s_sensors_ok, (long long)s_draw_ms, (unsigned)heap);
    NVSManager::getInstance().setString("ota_fail_why", why);
    NVSManager::getInstance().setU32("ota_bad", (uint32_t)FW_SEQ);

    // "does not return" is only true when there is somewhere to return TO. With no valid image in
    // the other slot the call reports an error and carries on, leaving this one PENDING_VERIFY —
    // so the next wake lands here again, and the next, rewriting both keys about 1440 times a day
    // against flash rated for a hundred thousand cycles. The image would grind the storage down
    // over a fault it cannot escape.
    //
    // If there is nowhere to fall back to, this image is the only one the device has. Keep it and
    // say so: the failed-boot counter is still watching, and the reason stays in flash for the
    // console to show. Refusing to run the only firmware present is not a safer outcome.
    const esp_err_t rb = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "rollback refused (%s) — no valid image to fall back to, keeping this one",
             esp_err_to_name(rb));
    esp_ota_mark_app_valid_cancel_rollback();
}

// ---- did the last boot finish? -------------------------------------------------------------
// First thing, before a single peripheral is touched, because the failure this catches is one
// that happens before anything else gets the chance to record it.
// Returns how many boots in a row have not reached sleep, this one included.
static uint8_t count_unfinished_boot(void)
{
    const uint32_t boot_guard_now = buildStamp() ^ 0xB007C0DEu;
    if (rtc_boot_guard != boot_guard_now) {      // new image, or RTC we cannot trust
        rtc_boot_guard = boot_guard_now;
        rtc_boot_incomplete = 0;
    }
    if (rtc_boot_incomplete < 255) rtc_boot_incomplete++;
    const uint8_t boot_fails = rtc_boot_incomplete;
    if (boot_fails > 1)
        ESP_LOGE(TAG, "boot %u without ever reaching sleep — this image may not be able to run",
                 (unsigned)boot_fails);
    return boot_fails;
}

// Five boots in a row that never reached sleep: back to the previous image, or into safe mode.
[[noreturn]] static void recover_from_failed_boots(uint8_t boot_fails)
{
    // Prefer going back to the image that used to work, through Espressif's rollback API
    // (esp_ota_ops.h): esp_ota_mark_app_invalid_rollback_and_reboot() marks this image INVALID and
    // restarts into the previous workable one; it refuses when esp_ota_check_rollback_is_possible()
    // finds none, and then safe mode below is the answer. The image marked INVALID is what
    // esp_ota_get_last_invalid_partition() reports, which is how the installer knows not to fetch
    // this version again (ota_install.cpp). No NVS and no peripheral here: this boot is failing.
    if (esp_ota_check_rollback_is_possible()) {
        ESP_LOGE(TAG, "rolling back to the previous image");
        rtc_boot_incomplete = 0;              // the other image gets its own clean count
        const esp_err_t rb = esp_ota_mark_app_invalid_rollback_and_reboot();   // returns only on failure
        ESP_LOGE(TAG, "rollback failed (%s) — safe mode", esp_err_to_name(rb));
        rtc_boot_incomplete = boot_fails;
    }
    static EPaper safe_epd;
    s_restart_panel = &safe_epd;
    safe_mode(safe_epd, boot_fails < BOOT_FAIL_BARE);   // never returns
}

// ---- power management ---------------------------------------------------------------------
// Wi-Fi mode: the power manager scales the clock and nothing else — 160 MHz while working, lower
// while every task is blocked, no automatic light sleep on the cable or on the battery. Matter mode
// (below the history of this decision) is the one place it sleeps lightly: a Thread end device
// cannot deep sleep, and esp-matter's icd_app enables automatic light sleep for exactly that.
//
// It used to be enabled on battery only, which made the two supplies run different firmware. A
// light sleep isolates every pad while it lasts (ESP_SLEEP_GPIO_RESET_WORKAROUND selects
// PM_SLP_DISABLE_GPIO): on battery the LED strobed while the button was polled, and the panel's
// P-FET gate could float through a wait. On hardware, 2026-09-11: after a factory reset on battery
// the button did not wake the logo sleep; after a boot on the cable it did, and went on doing so
// once the cable was pulled. The setting was chosen once per boot from the cable level, so pulling
// the cable later changed nothing. CONFIG_FREERTOS_USE_TICKLESS_IDLE is off in sdkconfig, so light
// sleep cannot come back through this call either.
//
// The price: the waits light sleep used to cover — the gas sensor's heater profile, the CO2
// conversion, the panel's BUSY — are now spent with the core idle instead of asleep.
static void configure_power_management(bool light_sleep)
{
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,            // the crystal's own frequency: the floor that keeps
                                       // peripherals clocked while the core idles
        .light_sleep_enable = light_sleep,
    };
    const esp_err_t pe = esp_pm_configure(&pm);
    ESP_LOGW(TAG, "power management: %s, clock 160/40 MHz, %s",
             pe == ESP_OK ? "on" : esp_err_to_name(pe),
             light_sleep ? "automatic LIGHT SLEEP (Matter mode)" : "no light sleep");
}

// Pins first, so the LED can answer the press immediately.
static void setup_led_and_button(void)
{
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, 0);
    gpio_hold_dis(PIN_USER_BTN);         // a pad left latched ignores every later config
    gpio_config_t btn = {};
    btn.pin_bit_mask = (1ULL << PIN_USER_BTN);
    btn.mode = GPIO_MODE_INPUT;
    btn.pull_up_en = GPIO_PULLUP_ENABLE;
    btn.intr_type = GPIO_INTR_NEGEDGE;          // pressed = pulled to ground
    gpio_config(&btn);
    // ESP_ERR_INVALID_STATE simply means the service is already there; nothing to report.
    const esp_err_t isr_svc = gpio_install_isr_service(0);
    if (isr_svc == ESP_OK || isr_svc == ESP_ERR_INVALID_STATE)
        gpio_isr_handler_add(PIN_USER_BTN, btn_isr, nullptr);
}

// What woke the device, as far as the button is concerned. Decided once, at the top of the boot.
struct ButtonWake {
    bool     by_button;      // a press: the wake mask, or a press caught during the previous cycle
    bool     btn_long;       // held for three seconds or more
    bool     btn_off;        // ...and held all the way to ten: the owner is asking to switch off
    bool     seen_down;      // a finger was on the button when the device woke
    uint32_t ghost_detail;   // a rejected button wake, for the flight recorder; 0 = none
};

static ButtonWake classify_button_wake(bool cold, uint64_t wake_pins)
{
    // Either the button woke the chip, or it was pressed during the previous cycle while the
    // device was too busy to notice and asked to be woken straight back up for it.
    // ...but only if that flag is ours. It is RTC_NOINIT, so a new image reads it at a moved
    // address and a cold start reads whatever the RAM held. Three sanity blocks further down clear
    // it — and all three were DEAD with respect to this variable, because it is consumed here,
    // above them, and cleared unconditionally two lines later. A stray nonzero byte therefore
    // manufactured a short press on the first boot of every firmware update: LED on, an
    // off-schedule CO2 reading, a full refresh burnt, and in hub mode a Wi-Fi session raised.
    // Bounded to one cycle, and it happens on every single update.
    const bool btn_flag_ours = (rtc_build_stamp == buildStamp()) && !cold;
    // A wake the chip puts down to the button is a press: the rule the running cycle has always had,
    // and the one a person expects.
    //
    // 0.9.40 and 0.9.41 also threw away any such wake that came less than two seconds after going to
    // sleep with no finger found on the button, and after eight of those arm_wake_pins() stopped
    // arming the button for fifteen minutes. That rule could reject a real second press, so it is gone.
    // It was NOT why the button failed to wake the logo sleep on battery (2026-09-11): that sleep
    // inherited a timer wake from the power manager — see arm_wake_pins().
    //
    // One bound is kept, and it cannot catch a person out: after PORTAL_UNUSED_MAX setup windows in a
    // row that opened on an unheld press and that nobody used, only a press still held when the device
    // wakes opens another. Somebody who wants the window uses it, or holds the button.
    bool mask_button = ((wake_pins & (1ULL << PIN_USER_BTN)) != 0);
    bool btn_seen_down = false;
    uint32_t btn_ghost_detail = 0;            // written to the flight recorder once it is open
    if (mask_button) {
        for (int i = 0; i < 30 && !btn_seen_down; ++i) {          // 300 ms, polled
            if (gpio_get_level(PIN_USER_BTN) == 0) btn_seen_down = true;
            else vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (btn_seen_down || rtc_portal_unused < PORTAL_UNUSED_MAX) {
            rtc_btn_ghosts = 0;
        } else {
            rtc_btn_ghosts++;
            ESP_LOGW(TAG, "button wake with nothing held after %u unused setup windows — ignoring; "
                          "hold the button to open another", (unsigned)rtc_portal_unused);
            btn_ghost_detail = 0x20000u | 0x40000u;
            mask_button = false;
        }
    }
    const bool by_button = mask_button || (rtc_btn_pending && btn_flag_ours);
    // Read and cleared together with the pending flag they belong to: a carried duration is only
    // meaningful for the press that set it, and a stray byte must not become a long press.
    uint32_t btn_carried = 0;
    if (rtc_btn_pending && btn_flag_ours) btn_carried = rtc_btn_held_ms;
    rtc_btn_held_ms = 0;
    if (rtc_btn_pending) {
        const bool was_ours = btn_flag_ours;
        rtc_btn_pending = false;
        if (!was_ours) {
            ESP_LOGW(TAG, "mid-cycle press flag ignored: it predates this image or this power-up");
        } else {
            // Named explicitly: a wake attributed to the button has two sources, and they fail
            // differently. The mask comes from the chip and cannot lie; this flag is our own
            // software deciding a press happened, and that is the one that was wrong.
            ESP_LOGW(TAG, "wake classified as BUTTON by the mid-cycle flag, not by the wake mask");
        }
    }

    // Classify the press right here, while it is still being held: boot takes about half a
    // second, so a finger is normally still down. Two blinks acknowledge it; passing 3 s turns
    // it into a LONG press and five blinks say "registered, you can let go".
    bool btn_long = false, btn_off = false;
    if (by_button) {
        // Steady, not flashing. The light means one thing — "your press was heard and I am working
        // on it" — and it says that best by simply being on from the press until the device is done.
        // Acknowledging with two flashes and then marking the long press with five more put seven
        // flashes into three seconds, which reads as a fault light rather than a confirmation.
        led(true);
        // Not zero: a press that began before this boot arrives with time already on its clock.
        int held = (int)(btn_carried < 60000u ? btn_carried : 0u);
        if (held) ESP_LOGW(TAG, "press continues from a previous cycle (%d ms already held)", held);
        while (gpio_get_level(PIN_USER_BTN) == 0 && held < 3000) {
            vTaskDelay(pdMS_TO_TICKS(20)); held += 20;
        }
        if (held >= 3000) {
            btn_long = true;
            // Three deliberate gaps in an otherwise steady light — the one moment that genuinely
            // needs a signal, since nothing else can tell the user their finger has been down long
            // enough to let go. Slow enough to count: three of these read as a message, where the
            // five fast flashes this replaced read as a fault lamp.
            for (int i = 0; i < 3; i++) {
                led(false); vTaskDelay(pdMS_TO_TICKS(150));
                led(true); vTaskDelay(pdMS_TO_TICKS(150));
            }
            held += 900;   // the three gaps are real time, and the next threshold counts from the press
            // A press that goes on past ten seconds is asking for something else entirely. The
            // threshold has to be far from three seconds, not near it: whoever is holding has
            // already been told "registered, you can let go", so everything past that point is
            // deliberate. Ten is also what a person does to a device they think has stopped
            // responding, which is exactly the moment switching it off is the right answer.
            while (gpio_get_level(PIN_USER_BTN) == 0 && held < BTN_OFF_MS) {
                vTaskDelay(pdMS_TO_TICKS(20)); held += 20;
                if ((held % 2000) == 0) esp_task_wdt_reset();
            }
            if (held >= BTN_OFF_MS) {
                btn_off = true;
                // One long dark, not three short ones: a different message needs a different
                // shape, and after three seconds of gaps a fourth gap would just read as more
                // of the same. This one says "let go, I heard you".
                led(false); vTaskDelay(pdMS_TO_TICKS(1200)); led(true);
            }
            wait_button_release(btn_off ? "power-off press" : "long press");
        }
        // Wait for the finger on a SHORT press too, and then throw away anything the interrupt
        // collected. Every edge up to this instant belongs to the press being handled right now;
        // treating one of them as a NEW press is what turned a single press into two cycles —
        // light, full refresh, sleep 50 ms, light, full refresh again. The edge comes from contact
        // bounce on RELEASE, which the noise filter cannot reject because during a bounce the line
        // really is at ground: it is a genuine edge belonging to a press we have already dealt with,
        // not interference. A short settle first, so the bouncing is over before the flag is cleared.
        wait_button_release("press");
        vTaskDelay(pdMS_TO_TICKS(60));
        if (s_btn_pressed_while_awake) {
            s_btn_pressed_while_awake = false;
            ESP_LOGW(TAG, "swallowed a bounce edge from this same press");
        }
        ESP_LOGW(TAG, "button: %s press (%d ms)", btn_long ? "LONG" : "SHORT", held);
        // How long it was actually held, and how it was classified. The open question about this
        // button is whether presses are being lost or invented, and only the held time separates
        // "the user let go early" from "the device never saw the press".
        EvtLog::getInstance().event(EVT_BUTTON,
            (uint32_t)(held & 0xFFFF) | (btn_long ? 0x10000u : 0u));
    }
    // Lit only when somebody is waiting for it — a press, or the setup window. A scheduled wake
    // passes in the dark: at night the device gets up every few minutes, and a light blinking in
    // a bedroom is a nuisance that also costs charge for nothing. Silence is the normal state
    // here, the same way the screen shows "!wifi" only when a sync has actually failed.
    if (by_button) led(true);
    return ButtonWake{by_button, btn_long, btn_off, btn_seen_down, btn_ghost_detail};
}

// Name the reset reason on every boot. The device was seen restarting when merely handled,
// and only this tells apart a supply dip (BROWNOUT), a disturbed enable pin (EXT), a crash
// (PANIC) and a genuine power-up — guessing from behaviour cannot.
static void log_reset_reason(void)
{
    const char *rr;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   rr = "POWERON (supply applied)";        break;
        case ESP_RST_EXT:       rr = "EXT (reset/EN pin pulled)";       break;
        case ESP_RST_BROWNOUT:  rr = "BROWNOUT (supply dipped!)";       break;
        case ESP_RST_PANIC:     rr = "PANIC (firmware crash)";          break;
        case ESP_RST_SW:        rr = "SW (esp_restart)";                break;
        case ESP_RST_DEEPSLEEP: rr = "DEEPSLEEP (normal wake)";         break;
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_WDT:       rr = "WATCHDOG";                        break;
        default:                rr = "other";                           break;
    }
    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, "RESET REASON: %s", rr);
    // Which clock is the sleep timer and the wall clock actually counting on? The build asks for
    // the external 32.768 kHz crystal, but if it fails to oscillate IDF falls back to an internal
    // RC oscillator without a word — and the only symptom is time drifting away over days. The
    // config states the intent; only the chip states the fact.
    {
        const soc_rtc_slow_clk_src_t src = rtc_clk_slow_src_get();
        uint32_t hz = 0;
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_RTC_SLOW,
                                     // CACHED, not EXACT: the exact form re-runs the
                                     // slow-clock calibration against the main crystal —
                                     // real milliseconds of awake time, on every wake, to
                                     // print a line that only ever changes at boot.
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz);
        ESP_LOGW(TAG, "RTC slow clock: %s = %u Hz",
                 (src == SOC_RTC_SLOW_CLK_SRC_XTAL32K) ? "XTAL32K (external crystal, as intended)"
               : (src == SOC_RTC_SLOW_CLK_SRC_RC32K)   ? "RC32K -- FALLBACK, crystal NOT running"
                                                       : "RC_SLOW -- FALLBACK, crystal NOT running",
                 (unsigned)hz);
    }
}

// Open the flight recorder the moment this wake's identity is known, so everything after this
// line has somewhere to be written down. init() is a partition lookup, and only after a real
// power loss does it also scan for the highest sequence number.
static void open_flight_recorder(esp_sleep_wakeup_cause_t cause, bool cold, bool by_usb,
                                 uint8_t boot_fails, const ButtonWake &bw)
{
    const bool by_button = bw.by_button;
    const uint32_t btn_ghost_detail = bw.ghost_detail;
    EvtLog &evt = EvtLog::getInstance();
    evt.init();

    // How much of this wake happened before the line above. awake_ms is measured from there, so
    // the bootloader, the ESP-IDF start-up and the seven steps ahead of this one have never been
    // counted — every "the device is awake N minutes a day" figure taken from this log, including
    // the energy budget argued from it on 2026-09-20, was a lower bound by exactly this much.
    // Self-limiting for the same reason as the light-sleep note: the first 64 wakes of an image
    // answer the question, and it re-arms on the next image.
    if (rtc_pre_notes > 64) rtc_pre_notes = 0;     // RTC_NOINIT garbage, as with rtc_ls_notes
    if (rtc_pre_notes < 64) {
        rtc_pre_notes++;
        uint32_t pre = (uint32_t)(evt.baseUs() / 1000);
        if (pre > 0xFFFFFF) pre = 0xFFFFFF;
        evt.event(EVT_NOTE, (uint32_t)NOTE_WAKE_PRE | (pre << 8));
    }
    evt.setWake(cold ? WK_COLD : by_button ? WK_BUTTON : by_usb ? WK_USB
                     : (cause == ESP_SLEEP_WAKEUP_TIMER) ? WK_TIMER : WK_RESTART,
                rtc_build_stamp != buildStamp());
    evt.setBootIncomplete(boot_fails);
    evt.setFlags(usb_present() ? EF_USB : 0);
    // Idempotent and already called on every tick; asked here so the record can say whether the
    // store that holds settings, credentials and calibration actually mounted this time.
    evt.setSensor(ES_NVS, NVSManager::getInstance().init());
    // A pin wake that turns out not to be a cable change is noise, and a run of them is a
    // storm. Counted here so the recorder carries it and the arming below can back off.
    if (by_usb) {
        const bool shown = rtc_prev_valid ? rtc_prev.usb : false;
        if (usb_for_frame(shown) == shown) {
            if (rtc_usb_spurious < 255) rtc_usb_spurious++;
            if (rtc_usb_spurious == USB_STORM)
                ESP_LOGW(TAG, "USB detect line woke us %u times without the cable changing — "
                              "not arming it until a scheduled wake settles this",
                         (unsigned)rtc_usb_spurious);
        } else {
            rtc_usb_spurious = 0;
        }
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        rtc_usb_spurious = 0;      // a scheduled wake got through: the storm, if any, is over
    }
    // Only an ABNORMAL reset earns a record of its own. The ordinary ones are already implied
    // by the wake summary, and one extra record per wake saying "woke from deep sleep" would
    // halve the retention to say nothing.
    const esp_reset_reason_t rrn = esp_reset_reason();
    if (rrn != ESP_RST_DEEPSLEEP && rrn != ESP_RST_POWERON && rrn != ESP_RST_SW)
        evt.event(EVT_BOOT, boot_detail(rrn, 0));
    else if (boot_fails > 1)
        evt.event(EVT_BOOT, boot_detail(rrn, 0x100u));   // a boot that never reached sleep
    // A rejected button wake, with how long the sleep had lasted: the number that says whether
    // phantoms are still happening, readable from the log without a serial port.
    if (btn_ghost_detail) evt.event(EVT_BUTTON, btn_ghost_detail);
}

// A different image from the one that went to sleep. RTC memory survives a reflash, but the new image
// reads it through its own layout, so the schedule, the alerting and the screen start over.
static void forget_previous_build(void)
{
    // Written before anything else this block does, because it is the record that divides the
    // log into "before this firmware" and "after it". Without it a ring spanning an update is
    // one undifferentiated stream: EF_FIRST marks that SOME image started, never which one,
    // so the first question after a bad update — did this build make it worse — had no answer
    // in the only data a battery device can hand back.
    EvtLog::getInstance().event(EVT_BOOT, boot_detail(esp_reset_reason(), 0x200u));
    rtc_build_stamp = buildStamp();
    rtc_prev_valid = false;              // next draw is a full refresh, from nothing
    rtc_ls_notes = 0;                    // the new image gets its own light-sleep evidence
    rtc_pre_notes = 0;

    // Every RTC variable has just moved, so the calibration has to come back from flash —
    // and this is the branch every customer's first update will take.
    restore_gas_from_flash("firmware changed");

    // The schedule goes with it. These live in RTC memory, which survives a reflash, and a
    // rebuild moves them to different addresses — where they read whatever the old layout
    // left behind. A stamp from the FUTURE is the dangerous one: it says "measured moments
    // ago", so the sensors are never woken and every reading stays at zero, indefinitely.
    rtc_last_meas = rtc_last_co2 = rtc_last_wx_try = rtc_last_fc = rtc_last_weather = 0;
    rtc_last_hist = 0; rtc_met_expires = 0;
    // The weather VALUES go with the stamps, and for the same reason. Zeroing only the stamps left
    // the readings themselves reading the old layout: the outdoor block is gated behind the city
    // string and mostly got away with it, but the strip guards each cell by its own sentinel, so
    // garbage there is read as a reading. 0.10.16 shipped without this line and put 999 in the UV,
    // rain and cloud cells on every device that updated over the air.
    forget_weather();
    // The charging ceiling's anchor, in another file but the same trap: zero bytes read as a known
    // 0 %, and the device on the charger showed "1 % at 4.15 V" (battery_monitor.h).
    BatteryMonitor::getInstance().forgetChargeAnchor();
    rtc_last_full = 0; rtc_partials = 0; rtc_ink_blocked = false; rtc_mqtt_disco_at = 0;
    rtc_btn_pending = false; rtc_btn_held_ms = 0;
    // A console request read through a moved layout is noise; a genuine one is lost with the image
    // it was made in, and the owner simply holds the button again.
    rtc_matter_console = 0;
    // Powered is the safe default whichever way the old bytes read: switching the rail ON is the
    // free direction (it rises fast from a low-impedance source), and any path that parks the
    // panel says so again on its way into the sleep.
    rtc_panel_powered = true;
    rtc_btn_ghosts = 0; rtc_portal_unused = 0; rtc_logo_shown = false;
    // And the alerting, for the same reason and with a nastier symptom. What the owner was
    // last told is one field; read through a layout that has moved it is simply whatever used
    // to live at that address, and a plausible-looking value there means the next time the
    // radio comes up — the moment somebody presses the button to open the console — the device
    // announces that a problem nobody was ever warned about has now resolved itself. Alerting
    // is edge-triggered, so a corrupt "what I last said" is not cosmetic: it manufactures an
    // edge that never happened.
    rtc_alert = AlertState{}; rtc_alert_tier = 0; rtc_alert_confirmed = 0;
    rtc_class = 0; rtc_air_class = 0; rtc_cls_txt[0] = '\0';
    // The words themselves too. These are RTC_NOINIT and were initialised on no path
    // at all: read through a moved layout they are arbitrary bytes, and the delivery
    // path would have sent them verbatim to somebody's phone.
    rtc_al_title[0] = rtc_al_text[0] = rtc_al_level[0] = '\0'; rtc_al_tries = 0;
    rtc_al_seq = rtc_al_hub = 0;
    rtc_wx_failed = false; rtc_blind_wakes = 0; rtc_co2_fails = 0;
    // The outlook: read through a moved layout its dates could match today and its bytes still
    // be noise. rtc_last_fc is zeroed above, so the next weather window fetches it again.
    memset(rtc_fc, 0, sizeof(rtc_fc));
    // Night bookkeeping, same class again. rtc_was_night exists only to catch the EDGE where
    // night ends, and that edge triggers a full anti-ghosting refresh. Read through a moved
    // layout it can be true on a device waking in broad daylight, so the first boot after every
    // update manufactured a "night just ended" and spent a full refresh on it.
    rtc_was_night = false; rtc_night_began = 0;
    // These two decide whether the device refuses to run at all, and neither was initialised
    // on any path. Read through a shifted layout after a reflash, or as whatever the RAM held
    // after a real power loss, the lockout flag can be true on a healthy cell — which on battery
    // is unrecoverable, since the check runs before the screen, the radio and the button.
    // RTC_NOINIT that nothing owns is not "false", it is arbitrary.
    rtc_lockout = false; rtc_lock_drawn = false;
    rtc_usb_spurious = 0;   // NOINIT: after an update it reads at a moved address, and a
                            // stray byte here would silently disarm the cable wake
    rtc_btn_noise = 0;
    // After a real power loss this is noise, and a stray match would wipe a working device. The
    // NVS copy of the request is what carries a genuine one across a power cut.
    rtc_factory_req = 0;
    // The Wi-Fi penalty too. It counts WAKES, so read through a moved layout it can
    // be any number — and a large one silences the radio for as long as it takes to
    // count down, on a device that was working perfectly before the update.
    rtc_wifi_fail = false; rtc_wifi_fails = 0; rtc_wifi_skip = 0;
    // And the night summary. Its window is what makes it valid at all, so garbage
    // there reports a night that never happened — to the hub, the morning after a
    // battery change.
    rtc_ns_from = rtc_ns_to = 0; rtc_night_began = 0;
    ESP_LOGW(TAG, "new firmware build — screen, schedule and alerting start from scratch");
}

// Same trap, without a rebuild: a real power loss leaves these full of noise. Anything in the
// future or from before this project existed cannot be a time we measured at.
static void drop_impossible_stamps(void)
{
    const time_t nowc = time(NULL);
    const time_t floor_t = 1600000000;                 // 2020
    time_t *stamps[] = { &rtc_last_meas, &rtc_last_co2, &rtc_last_wx_try,
                         &rtc_last_fc, &rtc_last_weather, &rtc_last_full, &rtc_last_hist };
    for (time_t *p : stamps)
        if (*p != 0 && (*p < floor_t || (nowc > floor_t && *p > nowc))) *p = 0;
}

// Sensors stay parked for the whole setup phase — no measuring until the device is actually
// configured. Done on every boot while unconfigured, because a power cycle wakes them again.
static void prepare_unconfigured(void)
{
    // A device that stopped being configured some other way than a factory reset, which
    // factory_wipe() above has already handled in full. RTC memory survives a restart, so
    // without this it would still remember its readings, the weather, the tick count and,
    // worst of all, the gas calibration of its previous life.
    if (rtc_magic == CYC_MAGIC || rtc_prev_valid) {
        ESP_LOGW(TAG, "unconfigured — clearing everything this device remembered");
        forget_rtc_state();
    }
    // The measurement log too — and this one is checked on every unconfigured boot, not
    // just the first. It lives in its OWN raw partition, so the factory reset that wiped
    // NVS did not touch a single record: five-day-old readings from a previous life were
    // still turning up on the graph. Erased here rather than in the request handler,
    // because 2 MB takes seconds and the browser would be left holding the connection.
    // Erased only on a device that has NEVER been set up — one out of its box, or one
    // just factory reset. "Not configured at this moment" is a different thing entirely,
    // and using it here meant a single failed join could destroy months of readings.
    //
    // The distinction matters because the log lives in its own flash partition: a factory
    // reset wipes NVS and would leave the previous owner's measurements behind, which is
    // why this erase exists at all. `provisioned` is in NVS, so a reset clears it too and
    // the erase still happens exactly when it should.
    uint8_t prov = 0;
    NVSManager::getInstance().getU8("provisioned", prov, 0);
    HistStore::getInstance().init();
    if (!prov && HistStore::getInstance().count() > 0) {
        ESP_LOGW(TAG, "never set up — erasing %u measurements left from before",
                 (unsigned)HistStore::getInstance().count());
        HistStore::getInstance().eraseAll();
        // A fourth store, and the same rule as the other three (CLAUDE.md 4.21): "erase
        // everything" on this device is a list of separate actions, not one call. The
        // flight recorder holds the previous owner's rhythm — when the room was occupied,
        // when the cable went in — which is exactly the kind of thing a reset must forget.
        EvtLog::getInstance().init();
        EvtLog::getInstance().eraseAll();
        // The crash dump goes too, and this is the security half of the reset. A dump is a
        // snapshot of RAM: the owner's Wi-Fi name was read out of one with `strings` during
        // an earlier investigation, and their password, weather key and bot token live in
        // the same memory. Factory Reset is exactly the moment a device changes hands —
        // resale, repair, a warranty return — so leaving 64 KB of the previous owner's RAM
        // in flash is the one thing that reset most obviously promises not to do. It never
        // erased it: the handler only wiped NVS, and the dump partition is not NVS
        // (4.21 — three separate stores, and this was the third one nobody cleared).
        {
            const esp_err_t ce = esp_core_dump_image_erase();
            ESP_LOGW(TAG, "crash dump partition erased: %s",
                     ce == ESP_OK ? "yes" : esp_err_to_name(ce));
        }
    } else if (prov && HistStore::getInstance().count() > 0) {
        ESP_LOGW(TAG, "not configured just now, but %u measurements are kept — this device "
                      "has been set up before", (unsigned)HistStore::getInstance().count());
    }
    ESP_LOGW(TAG, "parking the sensors until setup is done");
    park_sensors();
}

// Three ways in, and only two were named. The third is an update that moves the RTC
// layout: rtc_magic is then read at an address that belongs to something else, the
// comparison fails, and the cycle starts over. That is correct — the first tick after
// an update should measure and repaint in full — but it means `rtc_tick`, published as
// `sys.boots`, counts wakes SINCE THIS LAYOUT and is not an odometer. 4.61 used it as
// the independent control on a 16-day run, which was sound only because no update
// landed inside that window. Anyone spanning an update with it will read low.
static void start_cycle_from_scratch(void)
{
    rtc_magic = CYC_MAGIC; rtc_tick = 0;
    // The gas algorithm's clock is restored from flash rather than restarted. Its
    // saved state was written against this timeline, and handing it a clock that
    // begins again at zero is what makes it throw the state away.
    restore_gas_from_flash("power was lost");
    rtc_bsec_nvs_at = 0; rtc_bsec_nvs_acc = 255;
    rtc_last_weather = 0; rtc_pres_pa = 0; rtc_pres_miss = 0;
    rtc_last_meas = 0; rtc_last_co2 = 0; rtc_last_wx_try = 0; rtc_last_fc = 0; rtc_last_hist = 0;
    rtc_met_expires = 0;
    rtc_last_full = 0; rtc_partials = 0; rtc_ink_blocked = false; rtc_mqtt_disco_at = 0;
    rtc_btn_pending = false; rtc_btn_held_ms = 0;
    // Powered is the safe default whichever way the old bytes read: switching the rail ON is the
    // free direction (it rises fast from a low-impedance source), and any path that parks the
    // panel says so again on its way into the sleep.
    rtc_panel_powered = true;
    rtc_btn_ghosts = 0; rtc_portal_unused = 0; rtc_logo_shown = false;
    rtc_co2 = 0; rtc_temp = rtc_hum = rtc_iaq = rtc_soc = rtc_soc_raw = 0; rtc_acc = 0;
    rtc_co2_fails = 0;   // a wiped device is not a broken one
    rtc_wx_city[0] = rtc_wx_desc[0] = rtc_wx_icon[0] = 0;
    rtc_day_lo = 999.0f; rtc_day_hi = -999.0f; rtc_day_stamp = 0;
    memset(rtc_fc, 0, sizeof(rtc_fc));   // noise after a power loss; refetched next window
    rtc_wifi_fail = false; rtc_wifi_fails = 0; rtc_wifi_skip = 0;
    // Same reasoning as the build-stamp reset above: after a power loss this is noise,
    // and one byte of noise is enough to announce that a problem nobody was ever told
    // about has now resolved itself.
    rtc_alert = AlertState{}; rtc_alert_tier = 0; rtc_alert_confirmed = 0;
    rtc_al_title[0] = rtc_al_text[0] = rtc_al_level[0] = '\0'; rtc_al_tries = 0;
    rtc_al_seq = rtc_al_hub = 0;
    rtc_wx_failed = false; rtc_blind_wakes = 0; rtc_ns_from = rtc_ns_to = 0;
    ESP_LOGW(TAG, "first cycle after setup — measuring everything before drawing");
}

// The LOW BATTERY stop: the notice is painted once and left on the glass, and the device looks for a
// charger once an hour.
[[noreturn]] static void sleep_low_battery(EPaper &epd)
{
    if (!rtc_lock_drawn) {
        if (epd.init()) {
            draw_rle(epd, "LOW BATTERY", lock4g_plane24_rle, LOCK4G_PLANE24_RLE_LEN,
                     lock4g_plane26_rle, LOCK4G_PLANE26_RLE_LEN);
            rtc_lock_drawn = true;
        }
    }
    park_sensors();
    led(false);
    // Parked, like the logo: the picture is final and the hourly wakes only look for a
    // charger (the RUNNING / PARKED rule at the top; owner's decision 2026-09-12). A wake
    // that finds the supply already cut only re-latches the gate. The cable or a recovered
    // cell brings the device back through the working cycle, whose init() powers the panel.
    panel_to_sleep(epd, PanelSleep::Parked);

    arm_wake_pins(UsbWake::OnPlug);   // stopped: only a charger is worth waking for
    ESP_LOGE(TAG, "LOW BATTERY — sleeping 1 h (or until the cable / button)");
    enter_deep_sleep(3600ULL * 1000000ULL, true);   // look again in an hour
}

// The zone's offset now, from the POSIX rule the console stored (NVS "tz_rule", e.g.
// "STD-1DST,M3.5.0,M10.5.0/3"), written to tz_h when it changes — twice a year where there is daylight
// saving. Everything in this firmware computes local time as UTC + tz_h, so this is the one place that
// knows about seasons. The rule is newlib's TZ format (ESP-IDF "System Time": setenv("TZ") + tzset());
// the process is put back on UTC afterwards so nothing else changes behaviour. Without a rule, or
// without a clock, tz_h stays as the console or OpenWeatherMap left it.
static void follow_tz_rule(time_t now)
{
    if (now < 1600000000) return;
    std::string rule;
    NVSManager::getInstance().getString("tz_rule", rule, "");
    if (rule.empty()) return;
    setenv("TZ", rule.c_str(), 1);
    tzset();
    struct tm lt;
    localtime_r(&now, &lt);
    setenv("TZ", "UTC0", 1);
    tzset();
    const long long local = (long long)days_from_civil(lt.tm_year + 1900, (unsigned)(lt.tm_mon + 1),
                                                       (unsigned)lt.tm_mday) * 86400LL
                          + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    const long long off_s = local - (long long)now;
    if (off_s < -14 * 3600 || off_s > 14 * 3600) return;       // a rule newlib could not read
    const float off_h = (float)off_s / 3600.0f;
    float cur = 0;
    NVSManager::getInstance().getFloat("tz_h", cur, 0.0f);
    if (fabsf(cur - off_h) > 0.01f) {
        NVSManager::getInstance().setFloat("tz_h", off_h);
        ESP_LOGW(TAG, "time zone: UTC%+.2f -> UTC%+.2f by the zone's rule (%s)", cur, off_h, rule.c_str());
    }
}

// What one wake of the working cycle is going to do, decided before any of it is done.
struct Tick {
    time_t now;
    bool   clock_ok;
    Plan   plan;
    bool   first;          // first tick after setup, a power loss or a moved RTC layout
    bool   night_ended;
    bool   night_park;     // this sleep may take the panel's supply down with it
    bool   night_freeze;   // ...and THIS tick draws the last frame before it goes
    bool   due_full;       // a cleaning frame is due
    bool   do_meas;        // the gas period is up
    bool   do_co2;         // the CO2 interval is up
    bool   do_wx;          // a radio window is due
    bool   forced;         // a press: read CO2, go online and repaint whatever the schedule says
    bool   btn_long;       // ...and serve the console
};

static Tick plan_tick(const ButtonWake &bw)
{
    const bool by_button = bw.by_button, btn_long = bw.btn_long;
    const time_t now   = time(NULL);
    const bool clock_ok = now > 1600000000;
    follow_tz_rule(now);
    const Plan plan    = readPlan(now);
    const bool first   = (rtc_tick == 0);
    // Anti-ghosting full refresh — but not during the night. A full refresh inverts the
    // whole screen; in a dark bedroom that is a flash, four times a night, for a trace
    // that fifteen-odd partial updates barely accumulate. So the schedule is held over
    // until morning and spent in one go when the night ends. Worst case is a slightly
    // smudged picture by dawn, cleaned the moment it is light.
    const bool night_ended = rtc_was_night && !plan.night;
    // Through the night the supply may go down. Nobody is looking, nothing is being repainted,
    // and the switch the panel maker put on the board is there for exactly this: the rail costs
    // 33 uA through R9's gate pull-up alone, beside a sleep floor of some three hundred.
    // A press is the exception — somebody IS looking, so it repaints and the supply stays up.
    // Matter mode is excluded: it never deep-sleeps and never calls sleep_after_cycle(), so it
    // has no place to park and nothing to gain; leaving it in would only buy it a nightly
    // cleaning frame it did not ask for.
    const bool night_park = plan.night && !by_button && !plan.matter;
    // Which tick draws the last frame. The panel is still powered on the day-to-night edge, and
    // it is NOT on every night tick after that — which is exactly how the two are told apart,
    // with no extra state to go stale. `first` is in there for the device that just finished
    // setup at night: its panel was parked under the logo, so the flag says "already frozen"
    // when nothing has been frozen at all.
    const bool night_freeze = night_park && (rtc_panel_powered || first);
    // Nights used to be excluded from this entirely, so eight hours passed without a
    // single cleaning frame and the worst accumulation of the day was waiting at dawn —
    // which is what the morning photographs kept showing. The reason for excluding them
    // was that the cleaning frame is the one thing on this device that visibly interrupts,
    // and nobody wants a flash in a dark bedroom. That reason is much weaker now: the full
    // refresh is the fast waveform (4.67), and nobody is looking at 3 a.m. anyway.
    const bool due_full = first || !rtc_prev_valid || night_ended ||
                          // Every frame that is followed by a cut rail must be a FULL one.
                          // This is the rule written out in sleep_after_cycle(), established on
                          // this hardware and never yet wrong: a partial does not survive losing
                          // power, a full one does. night_freeze is the only tick that draws and
                          // then parks with a live baseline; the rest have !rtc_prev_valid from
                          // the previous park, which lands them here anyway.
                          night_freeze ||
                          // The partial count is the FALLBACK, for a device with no clock
                          // to schedule against. With a clock it was reached first — thirty
                          // partials is thirty minutes with a live clock.
                          // The COUNT is the vendor's rule and now applies always. It
                          // used to be the fallback for a device with no clock, and the
                          // two-hour timer took over as soon as the clock was set — which
                          // is how a rule about "every five partials" turned into one
                          // cleaning frame per sixty-two of them.
                          (rtc_partials >= FULL_EVERY_PARTIALS ||
                           (clock_ok && rtc_last_full > 0 &&
                            (now - rtc_last_full) >= FULL_EVERY_SEC));
    if (night_ended) {
        ESP_LOGW(TAG, "night over — clearing the night's ghosting");
        // While the radio was off the log kept running. This is the only chance to tell a
        // hub what it missed, and it is worth telling: the hours nobody is awake to look
        // are exactly the hours a bedroom monitor is for.
        if (clock_ok) {
            // The real start, when it is known. Twelve hours back is the fallback for a
            // device whose night began before the clock did.
            const time_t from = (rtc_night_began > 1600000000 &&
                                 (time_t)rtc_night_began < now)
                                  ? (time_t)rtc_night_began : (now - 12 * 3600);
            summarise_night(now, from);
        }
        rtc_night_began = 0;
    }
    if (plan.night && !rtc_was_night && clock_ok) rtc_night_began = (uint32_t)now;
    rtc_was_night = plan.night;

    // Two separate rhythms. The gas sensor keeps its fixed 300 s whatever the profile
    // says; the CO2 sensor follows the interval the Mode tab promises.
    //
    // Without a clock there is nothing to schedule against, and the comment that used to
    // stand here claimed the fallback "lasts one radio window". Nothing implemented that:
    // !clock_ok was a CONDITION, so a device that could not reach NTP — a power loss with
    // the router down, a network with no internet, an owner who set the device up without
    // Wi-Fi at all — took a FULL measurement on every 60 s wake, for ever. Five times the
    // rate the profile asks for, and the screen ticked away looking perfectly healthy while
    // the cell emptied in weeks. The boot-time stamp sanitiser guarantees it too: it zeroes
    // every stamp written while clockless, so rtc_last_meas == 0 comes back true each wake.
    //
    // So the blind bootstrap is bounded by the one thing available without a clock — a
    // count of wakes — and after that the cadence is kept by counting wakes as well.
    if (!clock_ok) { if (rtc_blind_wakes < 0xFFFFFF) rtc_blind_wakes++; }
    else            { rtc_blind_wakes = 0; }
    bool do_meas, do_co2;
    if (clock_ok) {
        do_meas = first || rtc_last_meas == 0 || (now - rtc_last_meas) >= GAS_SEC - GAS_EARLY_SEC;
        do_co2  = first || rtc_last_co2  == 0 ||
                  (now - rtc_last_co2) >= (time_t)plan.meas_min * 60;
    } else {
        // Blind. Every stamp written while clockless is below the sanity floor and gets
        // zeroed at the next boot, so no schedule kept in seconds can survive here — that
        // is precisely why the old condition never stopped being true. Count wakes instead,
        // and make each wake worth one gas period by sleeping GAS_SEC below.
        const uint32_t co2_every = (plan.meas_min > 5) ? (plan.meas_min / 5) : 1;
        do_meas = true;
        do_co2  = (rtc_blind_wakes <= 3) || (rtc_blind_wakes % co2_every) == 0;
    }
    // Is the clock merely old, or actually wrong? Three things say "wrong", and none of
    // them is caught by the routine weekly schedule:
    //   * it does not pass the sanity floor at all (power was lost);
    //   * the firmware was just replaced, so the whole RTC context is suspect;
    //   * it reads EARLIER than data this device has already written to its own log,
    //     which cannot happen to a clock that is running correctly.
    // The last one is the useful one: it needs no flag and no memory of what happened,
    // just a comparison against evidence the device produced itself.
    bool clock_suspect = false;
    {
        static bool asked = false;
        bool suspect = !clock_ok || rtc_tick == 0;
        if (!suspect) {
            // The newest timestamp this device has already written to flash. Flash is the
            // right place to look: it survives the power loss that wipes the clock, so it
            // is the one witness that outlives the problem being detected.
            HistStore::getInstance().init();
            uint32_t newest = 0;
            newest = HistStore::getInstance().newestTimestamp();
            if (newest > 0 && now + 60 < (time_t)newest) {
                ESP_LOGE(TAG, "clock reads %ld s BEFORE the newest stored reading — resyncing",
                         (long)((time_t)newest - now));
                suspect = true;
            }
        }
        clock_suspect = suspect;
        if (suspect && !asked) {
            asked = true;
            ClockSync::getInstance().forceClockSync();
        }
    }

    // Bounded the same way the blind measurement above is bounded, and for exactly the same
    // reason: while the clock is unset, every stamp written is below the sanity floor and
    // is zeroed at the next boot, so no schedule counted in seconds can ever advance and
    // the condition never stops being true. Unbounded, `!clock_ok` raised Wi-Fi on EVERY
    // wake — about once a minute, around the clock, since without a clock night is never
    // evaluated either — against the 32 sessions a day Optimal advertises. At the measured
    // ~20 s of active radio per session that is on the order of 100 mAh a day: a cell that
    // should last months is flat in about a fortnight.
    //
    // Reachable without anything being broken: a device whose network has no route to an
    // NTP server, or whose owner set it up on a LAN with no internet. It would simply run
    // itself down, and every reading it took would be stamped with a time it did not have.
    //
    // The first few wakes still get a free attempt, because that is how the clock is meant
    // to arrive. After that fall back to the profile's own cadence, counted in wakes rather
    // than seconds (a clockless wake sleeps one gas period, so five minutes to a wake). A
    // profile with the radio switched off gets the opening attempts and then stops: it is
    // configured never to go online, and its clock comes from the phone during setup.
    const uint32_t radio_every = (plan.radio_min > 5) ? (plan.radio_min / 5) : 1;
    const bool blind_radio = (rtc_blind_wakes <= 3) ||
                             (plan.radio_min > 0 && (rtc_blind_wakes % radio_every) == 0);
    // A clock known to be wrong earns a session of its own, night or not. forceClockSync()
    // only clears the "already synced" mark — it makes a sync happen once the radio is up,
    // it cannot bring the radio up. At night radio_min is zero, so a wrong clock that reads
    // as night locked itself in: no radio, therefore no NTP, therefore still wrong, for as
    // long as its own imagined night lasted. Everything measured in those hours went into
    // the log under a timestamp the device had already been told not to believe.
    const bool do_wx   = first || (!clock_ok && blind_radio) || clock_suspect ||
                         (plan.radio_min > 0 &&
                          (now - rtc_last_wx_try) >= (time_t)plan.radio_min * 60);
    publish_plan(plan);
    global_plan_last_meas  = (uint32_t)rtc_last_meas;
    global_plan_last_co2   = (uint32_t)rtc_last_co2;
    ESP_LOGW(TAG, "last CO2 attempt: %s | cached value %u ppm | stamped %ld s ago",
             rtc_co2_diag[0] ? rtc_co2_diag : "(none since power-up)",
             (unsigned)rtc_co2,
             (clock_ok && rtc_last_co2) ? (long)(now - rtc_last_co2) : -1L);
    ESP_LOGW(TAG, "profile %d%s | sensors every %u min | radio %s | clock %s",
             plan.profile, plan.night ? " (night)" : "", (unsigned)plan.meas_min,
             plan.radio_min ? "every few minutes" : "off",
             plan.live_clock ? "live" : "last reading");
    // A press means "show me the truth, now": read the CO2 sensor, fetch the weather,
    // re-sync the clock and repaint properly — whatever the schedule would have done.
    // The gas sensor is the one thing left alone: BSEC owns its 5-minute rhythm and an
    // off-beat reading breaks the accuracy it is building (CLAUDE.md 4.6).
    const bool forced = by_button;
    ESP_LOGW(TAG, "tick %u | sensors %s | weather %s%s", (unsigned)rtc_tick,
             (do_meas || forced) ? "yes" : "no",
             (do_wx || forced) ? "yes" : "no",
             forced ? (btn_long ? " | LONG press: web console too" : " | SHORT press: refresh")
                    : "");
    return Tick{now, clock_ok, plan, first, night_ended, night_park, night_freeze, due_full,
                do_meas, do_co2, do_wx, forced, btn_long};
}

// 1. sensors: read what is due, and record it in the history ring and the barometric forecast.
static void measure(const Tick &tick)
{
    const time_t now = tick.now;
    const bool do_meas = tick.do_meas, do_co2 = tick.do_co2, forced = tick.forced;
    float co2 = rtc_co2, t = rtc_temp, h = rtc_hum, pres = rtc_pres_pa;
    float iaq = rtc_iaq, soc = rtc_soc; uint8_t acc = rtc_acc;
    // The gauge legitimately reports past 100 % when the cell sits above its model's
    // full point — 101.8 % at 4.195 V was measured on the bench. True inside the chip,
    // nonsense on a screen, so it is clamped once, before anything displays it.
    const bool want_co2 = do_co2 || forced;
    bool co2_ok = false, bme_ok = false, bat_ok = false;
    const bool sensors_ok = read_sensors(co2, t, h, pres, iaq, acc, soc, want_co2, do_meas,
                                         &co2_ok, &bme_ok, &bat_ok);
    if (sensors_ok) {
        if (soc > 100.0f) soc = 100.0f;
        if (soc < 0.0f)   soc = 0.0f;
        rtc_co2 = (uint16_t)co2; rtc_temp = t; rtc_hum = h;
        rtc_iaq = iaq; rtc_acc = acc; rtc_soc = soc;
        EvtLog::getInstance().setGas((uint8_t)acc, (uint8_t)rtc_class, do_meas);
        EvtLog::getInstance().setSensor(ES_BSEC, do_meas && BSECManager::builtIn());
        EvtLog::getInstance().setBattery((uint16_t)lroundf(rtc_volt * 1000.0f),
            (uint16_t)((soc < 0 ? 0 : soc > 100 ? 100 : soc) * 100.0f));
        EvtLog::getInstance().setTemp(rtc_temp);
        // BSEC returning OK with no outputs at all is the failure that reads exactly
        // like a dead sensor on the glass (§4.6), so it gets named rather than implied.
        // A build without BSEC has no outputs by design, and logging that every five minutes
        // would bury the journal in a failure that is not one.
        if (do_meas && !(rtc_iaq > 0) && BSECManager::builtIn())
            EvtLog::getInstance().event(EVT_GAS, 0);
        // Back above a fifth of the cell: the warnings re-arm, so the next discharge
        // gets its own set. This is also what makes a swapped battery behave sensibly.
        if (rtc_soc >= 20.0f && rtc_bat_notified) {
            rtc_bat_notified = 0;
            ESP_LOGW(TAG, "battery back above 20 %% — low-charge warnings re-armed");
        }
    }
    // Into the history ring, which is what both graphs are drawn from — the one on
    // the glass and the one in the web console. Only with a real clock: a record
    // stamped before the first NTP sync would land at the wrong place in time.
    // The forecast is decided afresh on every measurement. A tick that cannot decide it — no clock,
    // no pressure, no reading three hours back — shows the number, not a Rain left from before.
    // Taken BEFORE the reset below: the hysteresis needs what the plaque shows now, and the reset is
    // what guarantees a tick that cannot decide falls back to the number instead of keeping a stale
    // Rain. Clamped because this lives in RTC_NOINIT and is whatever was in the register on the very
    // first power-up — garbage reading as "Storm" would hand that tick the loose threshold.
    const int8_t prev_mode = (rtc_pres_mode >= 1 && rtc_pres_mode <= 2) ? rtc_pres_mode : 0;
    rtc_pres_mode = 0; rtc_ptrend = 0;
    // Refreshed every measurement, whether or not a forecast comes of it: the console needs to say
    // why there is no word just as much as it needs to show one.
    global_baro_k = baro_factor(&global_baro_src);
    global_p_sea  = NAN;
    const time_t wall = time(NULL);
    if (wall > 1600000000) {
        HistStore::getInstance().init();
        // The step between records is the owner's setting now (NVS "h_step": 5, 10 or 15 minutes,
        // 15 by default — owner, 2026-09-16). It used to be the gas period by accident: the record was
        // written by every call of this function. Records go in on gas ticks only, so every one of them
        // carries the gas outputs, and 30 s of slack lets three ticks that each came two seconds early
        // still make fifteen minutes. The upper bound is the barometer below: it looks for a record
        // within 15 min of "three hours ago", so no step may exceed 30 min.
        uint8_t step_min = 15;
        NVSManager::getInstance().getU8("h_step", step_min, 15);
        if (step_min != 5 && step_min != 10 && step_min != 15) step_min = 15;
        // A wake where NOTHING answered writes no record at all. Every field below would come
        // from the previous cycle carrying this cycle's timestamp — a measurement that never
        // happened, indistinguishable afterwards from one that did. A gap is honest and every
        // consumer already handles it: records are minutes apart by design, and the graphs
        // filter on presence rather than assuming a fixed cadence.
        const bool hist_due = do_meas && sensors_ok &&
            (rtc_last_hist == 0 || wall < rtc_last_hist || wall - rtc_last_hist >= (time_t)step_min * 60 - 30);
        if (hist_due) {
            HistRec rec = {};
            rec.t      = (uint32_t)wall;
            // Only what was actually read this tick. A wake that measured nothing used to
            // write a full record assembled from the previous one, so the log gained a
            // reading that never happened — and the graphs drew it as data. Zero is the
            // sentinel every consumer already understands (4.17).
            // With a step longer than the CO2 interval, the CO2 sensor is read on ticks that write no
            // record. The newest reading since the last record goes into this one, so a step never loses
            // the CO2 line (the graph filters out records without it); an old one is not repeated.
            // Temperature and humidity come off the SAME read as the CO2, so they live or die
            // with it. They used to be written unconditionally, which meant a tick where the
            // SCD41 stayed silent logged the previous tick's room climate as this tick's.
            // `t` and `h` already hold the newest reading either way: the success branch above
            // assigns rtc_temp/rtc_hum from them, and they are initialised from those.
            const bool have_scd = co2_ok ||
                (rtc_last_co2 > rtc_last_hist && rtc_co2 > 0 && rtc_co2_fails == 0);
            rec.co2    = have_scd ? (co2_ok ? (uint16_t)co2 : rtc_co2) : 0;
            rec.temp_d = have_scd ? (int16_t)lroundf(t * 10.0f) : 0;
            rec.hum    = have_scd ? (uint8_t)(h < 0 ? 0 : h > 100 ? 100 : lroundf(h)) : 0;
            // 0 means "no reading", which is not the same as a pressure of zero — the web
            // graph breaks the line there instead of plotting a collapse (CLAUDE.md 4.17).
            rec.pres_d = (rtc_pres_pa > 30000.0f && rtc_pres_pa < 110000.0f)
                       ? (uint16_t)lroundf(rtc_pres_pa * 0.00750062f * 10.0f) : 0;
            // do_meas said "a gas tick was DUE", which is not the same as "the sensor answered".
            // On a gas tick the BME690 did not reply to, these carried the previous cycle's index
            // — the one number the whole air verdict is built on.
            rec.iaq    = bme_ok ? (uint16_t)lroundf(iaq < 0 ? 0 : iaq) : 0;
            rec.siaq   = bme_ok ? (uint16_t)lroundf(rtc_siaq < 0 ? 0 : rtc_siaq) : 0;
            // The BME690's own climate, for the self-heating question (hist_store.h). Same rule as the
            // gas outputs above: only on a tick that actually read the sensor.
            rec.b_temp_d = bme_ok ? (int16_t)lroundf(rtc_b_temp * 10.0f) : 0;
            rec.b_hum    = bme_ok ? (uint8_t)(rtc_b_hum < 0 ? 0 : rtc_b_hum > 100 ? 100
                                                               : lroundf(rtc_b_hum)) : 0;
            // Gas purity is a BSEC output like the two above, so it follows the same flag.
            rec.gas    = bme_ok ? (uint8_t)(rtc_gasp < 0 ? 0 : rtc_gasp > 100 ? 100
                                                             : lroundf(rtc_gasp)) : 0;
            // The gauge is a third device on the same bus and fails on its own schedule. Its
            // three fields already document 0 as "not recorded", so a silent gauge writes zero
            // rather than the last percentage it happened to report.
            // The gauge's own number, not the one on the glass: while the cable is in those two
            // differ on purpose, and a log that recorded the adjusted figure would be useless for
            // working out what this cell actually does.
            const float soc_log = bat_ok ? rtc_soc_raw : 0.0f;
            rec.soc    = bat_ok ? (uint8_t)(soc_log < 0 ? 0 : soc_log > 100 ? 100 : lroundf(soc_log)) : 0;
            rec.soc_d  = bat_ok ? (uint16_t)((soc_log < 0 ? 0 : soc_log > 100 ? 100 : soc_log) * 100.0f) : 0;
            rec.vcell_mv = bat_ok ? (uint16_t)lroundf(rtc_volt * 1000.0f) : 0;
            rec.flags  = (uint8_t)(((bme_ok ? acc : 0) & 0x3) | ((rtc_class & 0x7) << 2)
                                  | (usb_present() ? 0x20 : 0x00)
                                  | (bat_ok ? HIST_FLAG_CURVE : 0x00));
            EvtLog::getInstance().setSensor(ES_HIST,
                HistStore::getInstance().append(rec));
            rtc_last_hist = wall;
            ESP_LOGW(TAG, "history: %u records stored (a record every %u min)",
                     (unsigned)HistStore::getInstance().count(), (unsigned)step_min);
        }

        // ---- barometric forecast -------------------------------------------------
        // Pressure falls BEFORE a front arrives, so this looks 6-12 h ahead; it will
        // not show the rain already falling, because that drop has already happened.
        // Records with no pressure reading are skipped rather than read as zero — on a
        // unit whose barometer is dead that would look like a collapse and forecast a
        // storm out of nothing.
        //
        // The change is measured over three hours and named the way the Met Office names pressure
        // tendency (Analytics::baroMode). The reading compared is the one NEAREST to three hours ago:
        // the history query walks newest first, and taking its first match took the newest record in
        // the window, about 2 h 45 min back, which shrank every fall by a twelfth.
        if (rtc_pres_pa > 30000.0f && rtc_pres_pa < 110000.0f) {
            struct Ago { uint32_t t3; uint32_t off; float hpa; } ago = { (uint32_t)wall - 3 * 3600, 0xFFFFFFFFu, 0.0f };
            HistStore::getInstance().query(ago.t3 - 900, ago.t3 + 900, 40,
                [](const HistRec &r, void *ctx) {
                    Ago *a = (Ago *)ctx;
                    if (!r.pres_d) return;
                    const uint32_t off = (r.t > a->t3) ? r.t - a->t3 : a->t3 - r.t;
                    if (off < a->off) { a->off = off; a->hpa = r.pres_d / 10.0f / 0.750062f; }
                }, &ago);
            if (ago.hpa > 300.0f) {
                const float dp = rtc_pres_pa / 100.0f - ago.hpa;   // hPa over three hours
                const float p_sea = baro_sea_level(rtc_pres_pa / 100.0f);
                global_p_sea = p_sea;
                rtc_pres_mode = Analytics::baroMode(dp, p_sea, prev_mode);
                if (dp >  1.0f) rtc_ptrend =  1;
                if (dp < -1.0f) rtc_ptrend = -1;
                ESP_LOGW(TAG, "BARO: now %.1f, 3 h ago %.1f (%u s off), dp %+.1f hPa, "
                              "sea level %.1f -> %s",
                         rtc_pres_pa / 100.0f, ago.hpa, (unsigned)ago.off, dp,
                         isfinite(p_sea) ? p_sea : -1.0f,
                         rtc_pres_mode == 2 ? "STORM" : rtc_pres_mode == 1 ? "Rain" : "number");
            }
        }
    }

    // Only when the gas half actually ran. Stamping it on a button press told the
    // schedule a sample had been taken when none had, and the next real one was then a
    // full period away.
    if (do_meas) rtc_last_meas = now;
    // Only stamp it if a reading actually arrived. Stamping a failed attempt tells
    // the schedule the job is done, so the next try is a full interval away and the
    // screen keeps showing whatever it had — a zero, if it never succeeded at all.
    // The outcome of THIS attempt, not the value left over from an earlier one.
    // rtc_co2 > 0 is true for as long as the sensor has ever worked, so a device whose
    // SCD41 had just died went on stamping its schedule as satisfied — and the reading
    // on the glass stayed frozen at the last good number instead of ageing into dashes.
    // Exactly the failure this unit has actually had (CLAUDE.md 4.23).
    if (want_co2 && co2_ok) rtc_last_co2 = now;
    if (want_co2) {
        if (co2_ok) rtc_co2_fails = 0;
        else if (rtc_co2_fails < 255) rtc_co2_fails++;
    }

    // BSEC's clock moves on in read_sensors(), from the wall clock just before its call.
}

// 2. internet: time first, then weather.
// Would going online actually bring fresher outdoor data?
//
// This used to be a flat "younger than WX_FRESH_SEC", and that number was standing in for something
// it did not match. The real limit is the source's own: for MET the firmware asks nothing before
// the Expires of its last answer — their terms, honoured in fetch_own_weather() — and that Expires
// runs about half an hour ahead (measured against api.met.no, 2026-09-30), not fifteen minutes.
// Between the two a press raised the radio, was refused by the same gate, and came back with the
// numbers it already had, having spent some 2.3 s of radio: the second-largest item in the budget
// after the panel. Asking the gate the fetch itself asks closes that window by construction — the
// two can no longer drift apart, because there is only one of them.
//
// OpenWeatherMap publishes no Expires, so the fifteen minutes stand there; its current-conditions
// data moves on roughly that scale.
//
// The source is resolved exactly as fetch_own_weather() resolves it, key and all: a rule that
// decided by a different source than the one about to be asked would be worse than no rule.
//
// One thing this deliberately costs: a press inside the window no longer polls Telegram commands
// either, since the whole session is skipped. That was already true below fifteen minutes; the
// window is now wider. A press is "show me the readings", and commands ride the scheduled windows.
static bool weather_is_current(time_t now, bool clock_ok)
{
    if (!clock_ok || rtc_last_weather <= 0) return false;

    NVSManager &nv = NVSManager::getInstance();
    std::string owm;
    nv.getString("owm", owm, "");
    uint8_t src = WX_SRC_MET;
    nv.getU8("wx_src", src, WX_SRC_MET);
    if (src == WX_SRC_OWM && owm.empty()) src = WX_SRC_MET;   // no key: MET needs none

    if (src == WX_SRC_MET) {
        // The same guard fetch_own_weather() uses: an Expires further out than six hours is not a
        // promise this code wrote, so it is not honoured.
        return rtc_met_expires > now && (rtc_met_expires - now) < 6 * 3600;
    }
    return (now - rtc_last_weather) < WX_FRESH_SEC;
}

static void radio_window(const Tick &tick)
{
    const time_t now = tick.now;
    const Plan &plan = tick.plan;
    const bool clock_ok = tick.clock_ok, do_wx = tick.do_wx, forced = tick.forced,
               btn_long = tick.btn_long;
    // Raising the radio is by far the most expensive thing in this cycle, and it used to
    // happen on EVERY measurement tick — 288 joins a day, of which some 275 had nothing to
    // do: the clock re-syncs once a day and the weather every half hour. So the join now
    // happens only when something actually needs the network. NTP rides along in those
    // windows, which is far more often than once a day.
    // A press means "show me the truth now", and the CO2 sensor is read for it regardless.
    // The network is different: going online is the most expensive thing in this cycle, so it
    // only earns its keep if the source would actually hand back something newer.
    // A long press still connects — it is about to serve the web console.
    const bool wx_fresh = weather_is_current(now, clock_ok);
    // A warning or worse gets the radio raised off-schedule — including at night, which is
    // the whole point: a bedroom filling with CO2 at three in the morning is exactly when
    // nobody is going to press a button. Advisory findings do not; they wait their turn.
    // rtc_alert_tier is now the tier of a message that is OWED, and it is cleared the
    // moment one is sent. It used to hold the tier of the current situation, which is a
    // condition that stays true for hours: the device then went online on every wake for
    // as long as the air was bad, mostly to decide it had nothing new to say. One event,
    // one session.
    // Normally only a warning earns an unscheduled session and an advisory rides along
    // with the next window. On a profile with no scheduled window at all — Ultra Save —
    // "the next window" never comes, so an advisory would wait for ever while the card
    // promises the device speaks up. There it earns its own session: one per event, not
    // one per wake, because this is edge-triggered.
    const bool alert_owed  = (rtc_alert_confirmed != rtc_alert.notified_class);
    // plan.offline, NOT radio_min == 0. The intent is "this profile never goes online, so
    // an advisory has no window to wait for and must earn its own". But radio_min is zeroed
    // every night on EVERY profile, so the test was also true between the small hours and
    // morning — and an advisory is exactly the tier that is documented to wait (analytics.h).
    // The device therefore woke its radio and pushed "stuffy" or "stale" to the owner's
    // phone at three in the morning, which is the one thing the night profile exists to
    // prevent. plan.offline comes from the configured interval and is not touched by night.
    // The FIRST alert this device ever sends earns its own session too, whatever its tier.
    // Not for the air's sake — an advisory genuinely can wait half an hour — but because
    // until one message has arrived, the owner has no way to tell a quiet room from a
    // broken channel. That ambiguity is what this costs: on this very device the owner set
    // notifications up, waited, saw nothing, and reasonably concluded they were not working.
    // Silence has to be earned by a channel that has proved itself at least once.
    //
    // Deliberately NOT at night: a first alert is still not worth waking somebody at 03:00,
    // and by morning it will go out on the ordinary schedule. One extra session, once per
    // device lifetime — the flag is in NVS, so a factory reset arms it again for the next
    // owner, which is exactly when it is needed.
    uint8_t alerted_before = 0;
    NVSManager::getInstance().getU8("al_first", alerted_before, 0);
    const bool first_alert = !alerted_before && !plan.night;

    const bool alert_radio = alert_owed &&
                             (rtc_alert_tier >= TIER_WARNING || plan.offline || first_alert);
    // A low cell earns a session too, and this is the one case where spending charge to
    // report is clearly worth it: the alternative is a device that quietly runs itself flat
    // and stops, with the one message that could have prevented it never sent. It happens
    // three times per discharge at most.
    const int bat_lvl_now = battery_alert_level();
    const bool bat_radio = (bat_lvl_now != 0);
    // A press with a hub connected always goes online, even when the weather is fresh.
    // The old rule only asked whether there was anything to FETCH — written when weather
    // was the only reason to raise the radio. With MQTT there is something to SEND: the
    // person pressed the button to see current readings, and readings that never leave the
    // device are not current anywhere else. One session per press is what they asked for.
    uint8_t wl_now = 0;
    NVSManager::getInstance().getU8("wl_mode", wl_now, 0);
    // Somebody holding the button has quite likely just fixed whatever was wrong with the
    // network — a rebooted router, a corrected password. Clear the penalty and let them
    // see the result now rather than in an hour.
    if (btn_long) { rtc_wifi_skip = 0; rtc_wifi_fails = 0; }
    // Sit out the backoff a failed join earned. Only the SCHEDULED reasons are held: a
    // press, an air alert and a low battery are all somebody waiting on a message, and they
    // are rare enough that letting them try costs little even against a dead network.
    // Above the backoff table's own maximum is provably not a value this code wrote:
    // it is noise from a previous RTC layout, not a penalty to serve.
    if (rtc_wifi_skip > 60) { rtc_wifi_skip = 0; rtc_wifi_fails = 0; }
    bool wifi_held_off = false;
    if (rtc_wifi_skip > 0) { rtc_wifi_skip--; wifi_held_off = true; }
    // The backoff covers the urgent reasons too, and that matters most exactly where it
    // was missing: a low cell with an unreachable router used to retry a full join —
    // three seconds of fast-connect and ten of scanning — on every single wake, draining
    // the very charge it was trying to report. An owed alert did the same. Both still get
    // through: the backoff tops out at sixty wakes, so they are delayed, never dropped.
    // A long press is the one exception, because somebody is standing at the device.
    const bool want_radio = do_wx || (forced && (!wx_fresh || wl_now == 1))
                                  || alert_radio || bat_radio;
    // A device with no radio at all must still CLOSE its events, or the flags that ask for
    // a session stay raised for the life of the cell and every wake asks again. Nothing is
    // lost: the verdict is on the glass, and the hub — if there ever is one — reads the
    // state message, not the alert queue.
    if (plan.no_net) {
        if (rtc_alert_confirmed != rtc_alert.notified_class) {
            ESP_LOGW(TAG, "no network configured — closing air event locally (class %d)",
                     rtc_alert_confirmed);
            Analytics::getInstance().alertSent(rtc_alert, rtc_alert_confirmed, time(NULL));
            // detail: class in the low byte, tier in the next, the CO2 that tripped it above
            EvtLog::getInstance().event(EVT_ALERT,
                (uint32_t)(rtc_alert_confirmed & 0xFF) |
                ((uint32_t)(rtc_alert_tier & 0xFF) << 8) |
                ((uint32_t)rtc_alert_co2 << 16));
            rtc_alert_tier = 0;
        }
        const int lvl_off = bat_lvl_now;
        if (lvl_off) {
            if (lvl_off <= BAT_WARN_1) rtc_bat_notified |= 1;
            if (lvl_off <= BAT_WARN_2) rtc_bat_notified |= 2;
            if (lvl_off <= BAT_WARN_3) rtc_bat_notified |= 4;
        }
        rtc_al_tries = 0;
    }
    const bool need_radio = (want_radio && !wifi_held_off && !plan.no_net) || btn_long;
    if (wifi_held_off && (do_wx || forced))
        ESP_LOGW(TAG, "network unreachable — skipping this window, %u wake(s) to go",
                 (unsigned)rtc_wifi_skip);
    if (alert_radio && !do_wx) ESP_LOGW(TAG, "alert — raising the radio off-schedule");
    if (bat_radio && !do_wx)
        ESP_LOGW(TAG, "battery at %.0f %% — raising the radio to warn", rtc_soc);
    if (forced && wx_fresh && !btn_long && !do_wx && wl_now != 1)
        ESP_LOGW(TAG, "weather is %d min old and the source has nothing newer yet — "
                      "press handled without the radio", (int)((now - rtc_last_weather) / 60));
    if (need_radio) {
        // Timed here rather than inside go_online(): what matters to the budget is how
        // long the radio was actually up, and this is the boundary that owns that.
        const int64_t radio_t0 = esp_timer_get_time();
        s_console_session = btn_long;
        go_online(do_wx || forced);
        {
            const int64_t ms = (esp_timer_get_time() - radio_t0) / 1000;
            int8_t rssi = 0;
            if (WiFiManager::getInstance().isConnected())
                rssi = (int8_t)WiFiManager::getInstance().getDetails().rssi;
            EvtLog::getInstance().setRadio((uint16_t)(ms > 65535 ? 65535 : ms), rssi);
            EvtLog::getInstance().setSensor(ES_WIFI,
                WiFiManager::getInstance().isConnected());
            // The total is already in the record's own radio_ms field, so detail carries what
            // nothing else did: where the connect time went. Bit 31 marks the new encoding —
            // records written before 2026-09-20 hold plain milliseconds and never set it, so old
            // logs keep reading correctly. A window longer than the field can hold (a console
            // session) keeps the old plain form, because then the field is saturated and the
            // total is the only number worth having.
            uint32_t rd;
            if (ms <= 65535) {
                WiFiManager &wm = WiFiManager::getInstance();
                auto sat12 = [](uint32_t v) -> uint32_t { return v > 4095u ? 4095u : v; };
                rd = 0x80000000u
                   | sat12(wm.lastAssocMs())
                   | (sat12(wm.lastDhcpMs()) << 12)
                   | (wm.lastFastUsed() ? (1u << 24) : 0u)
                   | (wm.lastFastOk()   ? (1u << 25) : 0u);
            } else {
                rd = (uint32_t)ms;
            }
            EvtLog::getInstance().event(EVT_RADIO, rd);
        }
        // Keep the radio up only for a long press, which is about to serve the console.
        // Otherwise take it down NOW: the panel needs that memory to receive the frame,
        // and with Wi-Fi holding its buffers the transfer silently does not happen.
        if (!btn_long) {
            WiFiManager::getInstance().shutdown();
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
}

// The glass keeps its frozen frame this wake. ONE definition, asked by two places that must not
// disagree: whether to power the panel's rail at all, and whether to draw. They were separate for
// one night and that was enough — see the note where the rail is switched on.
//
// The alert block is the one thing worth waking for (CLAUDE.md, 2026-09-15), so it is part of the
// question rather than an exception bolted on after it.
static bool night_frozen(const Tick &tick)
{
    const bool alert_now = (rtc_class == 1 || rtc_class == 3 || rtc_class == 5);
    return tick.night_park && !tick.night_freeze && alert_now == rtc_prev.aqInv;
}

// What the glass ends up showing, and the mode the sleep has to be scheduled for.
struct Shown {
    int  drew;           // draw_dashboard(): 0 no repaint, 1 partial, 2 full
    bool full;           // a full refresh was asked for
    bool live_clock;     // the mode running once any console window has closed
};

// 3-5. The dashboard; on a long press the console too, on the home network or on the device's own
// access point.
static Shown draw_and_serve(EPaper &epd, const Tick &tick)
{
    const Plan &plan = tick.plan;
    const bool btn_long = tick.btn_long, forced = tick.forced, first = tick.first,
               night_ended = tick.night_ended, due_full = tick.due_full;
    // 3. long press: leave the network up and serve the console, with its address on the
    //    glass so the user knows where to go. Short press and scheduled ticks skip this.
    char addr[40] = {0};
    // A long press means "let me at the settings", and there are two ways to grant it.
    // On the home network the console goes up at the router's address and the dashboard
    // carries a band telling the user where to point a browser.
    bool own_ap = false;
    if (btn_long && WiFiManager::getInstance().isConnected()) {
        WebServer::getInstance().start();
        snprintf(addr, sizeof(addr), "smartmeteo.local (%s)",
                 WiFiManager::getInstance().getIPAddress().c_str());
        ESP_LOGW(TAG, "web console up at %s", addr);
    } else if (btn_long) {
        // No network to join — the saved one is out of range, or gone, or the user is
        // moving the device to a different one entirely. Falling back to the device's own
        // access point is the only way in: without it a configured device whose network
        // disappeared could never be reconfigured again.
        own_ap = true;
        ESP_LOGW(TAG, "no network — the setup access point will follow the QR screen");
        // The radio deliberately does NOT go up here; it waits until the QR picture is on
        // the glass. See the note at 5a.
    }

    // 4. now, and only now, put it on the glass. A press always gets a full refresh —
    //    the user is standing there waiting for it, so it should look clean, not like a
    //    partial update laid over the old figures.
    const bool full_now = due_full || forced;
    // Name the reason. Two full refreshes on consecutive night ticks were observed and
    // could not be explained from the code: the scheduled one is excluded at night, and
    // both one-shot invalidations (a changed build, an unconfigured device) had already
    // been spent. A cleaning frame is the one thing on this device that visibly
    // interrupts — a flash in a dark room — and it costs several times what a partial
    // does, so an unexplained one is worth a word in the log rather than a theory.
    if (full_now)
        ESP_LOGW(TAG, "full refresh because: %s%s%s%s%s",
                 first        ? "first tick "        : "",
                 !rtc_prev_valid ? "frame invalid "  : "",
                 night_ended  ? "night ended "       : "",
                 forced       ? "button "            : "",
                 (!first && rtc_prev_valid && !night_ended && !forced) ? "schedule" : "");
    // Skipped entirely when the QR screen is about to replace it: painting a dashboard that
    // is overwritten a second later costs a full refresh and shows the user a flicker of
    // something they did not ask for.
    // ...and, once the night's picture is frozen, not at all. Waking the panel to repaint
    // figures nobody is awake to read would spend the rail crossing, the frame and the two-second
    // settle, and then park it all over again. A press is not this branch: it clears night_park.
    //
    // One thing is worth waking for, and it is the one the owner asked not to wait for morning
    // (CLAUDE.md, 2026-09-15): the alert block flipping. aqInv is inverted for exactly the three
    // classes that mean something is wrong — Stuffy, polluted, Damp — so comparing it against
    // what is ON the glass wakes the panel when the verdict crosses that line, and stays quiet
    // for benign drift between Bosch bands. The repaint is full: night ticks reach here with
    // rtc_prev_valid already false, cleared by the park that froze the picture.
    const bool frozen = night_frozen(tick);
    if (frozen)
        ESP_LOGW(TAG, "night: the glass keeps its frozen frame, the panel stays off");
    int drew = (own_ap || frozen)
             ? 0
             : draw_dashboard(epd, full_now, plan.night, plan.offline, plan.live_clock,
                              addr[0] ? addr : nullptr);


    // The sleep below is scheduled for the mode that will be running, which a console window
    // may have just changed — the closing repaints re-read the plan for the same reason.
    bool live_clock_next = plan.live_clock;

    // 5a. Own access point: the glass shows the QR setup screen instead of the dashboard,
    //     exactly as it does out of the box — same picture, same instructions, and the
    //     phone joins the same network the QR encodes. A short press ends it early and the
    //     dashboard comes back.
    if (own_ap) {
        // The picture goes on the glass BEFORE the radio comes up, and the order matters.
        // A four-gray frame is 15 KB pushed over SPI and needs DMA-capable memory to do it;
        // Wi-Fi takes about ninety of the free kilobytes, and when there is not enough the
        // transfer simply does not happen — measured at 323 ms against a healthy 7358, with
        // nothing on the screen and no error raised anywhere. Drawing first costs a second
        // of ordering and removes the risk. Stopping the radio instead is not an option
        // here: this screen exists to say which network to join.
        {
            const bool usb = usb_present();
            const PortalScreen::Battery bat = shown_battery(usb, charge_finished(usb));
            draw_setup_screen(epd, "QR SETUP", &bat);
        }
        radio_up();
        // No address band here: the glass is showing the QR picture, not a dashboard, and
        // there is no router address to print — this is the path taken when there is no
        // network to join.
        hold_setup_window(epd, nullptr);
        radio_down();
        if (s_window_poweroff) enter_power_off(epd);   // never returns
        if (s_window_reboot) restart_from_window(epd);
        // The QR picture replaced the dashboard on the glass, so the controller's idea of
        // the previous frame is worthless: repaint from scratch, not from a stale baseline.
        rtc_prev_valid = false;
        live_clock_next = repaint_after_window(epd, plan, "setup window", drew).live_clock;
    }

    // 5b. long press on the home network: stay awake and serve the console, then close and
    //     repaint without the address band so the screen matches reality again.
    if (addr[0]) {
        // The address goes in: while this window is open the console really is reachable
        // there, so any repaint that happens inside it — applying a new mode, for instance
        // — has to keep the band. Leaving it out was what made it vanish on Apply.
        hold_setup_window(epd, addr);
        WebServer::getInstance().stop();
        // The update go_online() held back for this console, now that it has closed. The network is
        // still up; a restart the owner asked for comes first, and the next session installs it.
        if (!s_window_reboot) install_update_if_due();
        WiFiManager::getInstance().shutdown();
        vTaskDelay(pdMS_TO_TICKS(150));
        if (s_window_poweroff) enter_power_off(epd);   // never returns
        if (s_window_reboot) restart_from_window(epd);
        live_clock_next = repaint_after_window(epd, plan, "console", drew).live_clock;
    }
    return Shown{drew, full_now, live_clock_next};
}

// How long to sleep: whole seconds, and the part of a second on top.
struct NextWake {
    uint32_t s;
    int32_t  adj_ms;
};

// ---- when to wake up next ----------------------------------------------------------------------
// A flat 60 s from here would glue the update to the moment we woke, which drifts by
// the length of every wake: the minute would appear on the glass at :00.6, then :01.7,
// then :02.9. So the sleep is measured from the WALL CLOCK to the next minute, and we
// get up EARLY by however long this device actually takes to go from timer to settled
// pixels — a figure it measures about itself rather than assumes.
// `cycle_t0_us` is when this tick began by esp_timer: 0 for a wake out of deep sleep (the boot is part
// of the tick), the loop's own start in Matter mode.
static NextWake time_to_next_wake(const Tick &tick, bool full_now, bool live_clock_next,
                                  int64_t cycle_t0_us = 0, int drew = -1)
{
    const time_t now = tick.now;
    const bool clock_ok = tick.clock_ok, do_meas = tick.do_meas, forced = tick.forced;
    const uint32_t lat_ms = (uint32_t)((esp_timer_get_time() - cycle_t0_us) / 1000);   // this tick, so far
    const bool measured_now = (do_meas || forced);
    {
        // Full refreshes are four times slower than partials; letting one into the average
        // would poison the estimate for the next hundred ticks.
        uint32_t &slot = measured_now ? rtc_lat_meas_ms : rtc_lat_clock_ms;
        // A tick that drew NOTHING is an outlier in the other direction, and since 0.10.11 there
        // is a whole class of them: every night tick between the frozen frame and morning skips
        // the panel entirely. Feeding those in drags the measuring-tick average down by the cost
        // of a frame, and the first morning ticks then land late on the minute. Same reasoning as
        // the full refresh above — the average is meant to describe an ordinary tick.
        const bool poison = (!measured_now && full_now) || drew == 0;
        if (!poison && lat_ms > 0 && lat_ms < 30000) {
            slot = (slot == 0 || slot > 30000) ? lat_ms : (slot * 3 + lat_ms) / 4;
        }
    }

    uint32_t sleep_s = TICK_SEC;
    int32_t  sleep_adj_ms = 0;
    if (!live_clock_next || !clock_ok) {
        // Nothing to line up with — either the glass shows no clock, or there is no clock
        // to show. The second case used to fall through to the minute tick below and wake
        // this device sixty times an hour with no minutes to display and no schedule to
        // keep, which is what made the blind bootstrap so expensive.
        const time_t due = rtc_last_meas + GAS_SEC;
        sleep_s = (clock_ok && due > now) ? (uint32_t)(due - now) : GAS_SEC;
        if (sleep_s < 5) sleep_s = 5;
        if (sleep_s > GAS_SEC) sleep_s = GAS_SEC;
    } else if (clock_ok) {
        struct timeval tv; gettimeofday(&tv, NULL);
        // How far past the minute did the pixels actually settle? Past the half-minute
        // means we were early for the NEXT one, which is a negative error, not a huge one.
        int32_t err_ms = (int32_t)((tv.tv_sec % 60) * 1000 + tv.tv_usec / 1000);
        if (err_ms > 30000) err_ms -= 60000;
        // Half the residual, each tick. Everything before app_main exists — ROM loader,
        // flash init — cannot be measured from inside, so it is cancelled rather than
        // modelled. Clamped so one bad reading cannot run away with the schedule.
        rtc_sync_trim_ms -= err_ms / 2;
        if (rtc_sync_trim_ms >  2000) rtc_sync_trim_ms =  2000;
        if (rtc_sync_trim_ms < -2000) rtc_sync_trim_ms = -2000;

        // Will the next wake be a measuring one? It takes seconds longer, so the head start
        // has to match the tick we are actually about to run.
        const bool next_measures = (rtc_last_meas + GAS_SEC - GAS_EARLY_SEC) <= (tv.tv_sec + TICK_SEC);
        uint32_t lead = next_measures ? rtc_lat_meas_ms : rtc_lat_clock_ms;
        if (lead == 0 || lead > 30000) lead = next_measures ? 6000 : 1200;   // first time

        const int64_t to_boundary_ms = 60000 - ((int64_t)(tv.tv_sec % 60) * 1000
                                                + tv.tv_usec / 1000);
        int64_t want_ms = to_boundary_ms - (int64_t)lead + rtc_sync_trim_ms;
        while (want_ms < 5000) want_ms += 60000;      // already past it: aim at the next
        sleep_s = (uint32_t)(want_ms / 1000);
        sleep_adj_ms = (int32_t)(want_ms % 1000);
        ESP_LOGW(TAG, "clock sync: landed %+d ms off the minute | lead %u ms | trim %+d ms",
                 (int)err_ms, (unsigned)lead, (int)rtc_sync_trim_ms);
    }
    return NextWake{sleep_s, sleep_adj_ms};
}

// Leave the panel asleep with its supply on, arm the button and the cable, and sleep until the next
// tick, or for 50 ms when a press arrived during this one.
[[noreturn]] static void sleep_after_cycle(EPaper &epd, const NextWake &next, int drew, bool park)
{
    const uint32_t sleep_s = next.s;
    const int32_t sleep_adj_ms = next.adj_ms;
    // Two sequences work on this hardware, and which one applies depends on the last
    // refresh. Everything observed fits this and nothing else:
    //   full refresh   + power cut + deep sleep -> clean (the logo screen, for hours)
    //   partial        + power cut + AWAKE      -> clean (five cycles, measured)
    //   partial        + power cut + deep sleep -> GREYS
    //   partial        + powered   + deep sleep -> clean
    // So a partially refreshed image survives losing power, and it survives deep sleep,
    // but not both at once. After a full refresh the supply can go; after a partial it
    // stays. A wake that drew nothing leaves the switch exactly as it found it.
    // Whether the supply may go depends on how the last frame was written, and on nothing
    // else. Established on hardware once the line-clamping bug was out of the way:
    //     full refresh    + supply cut -> clean
    //     partial refresh + supply cut -> greys, every time
    //     partial refresh + supply kept -> clean
    // It matches what the manufacturer's own driver encodes: a full update (0x22=0xD7)
    // shuts the analog stage down and marks the panel powered off, while a partial one
    // (0x22=0xFC) leaves it running. The differential waveform does not drive pixels all
    // the way to their rails; the powered controller holds them there, and an unpowered
    // one cannot. Cutting the supply after every frame would therefore mean making every
    // frame a full refresh — which flashes the whole screen, once a minute, forever.
    // The supply STAYS ON through every sleep, whatever was just drawn.
    //
    // It used to be cut after a full refresh, on the reasoning that a fully driven image
    // needs no holding. The device disagreed: partial ticks, which keep the supply, held
    // their picture for hours, while every full refresh greyed the glass the moment it
    // slept. That is the whole experiment, run by the firmware itself on both branches at
    // once — and the three places that cut power were exactly the three static screens
    // meant to survive longest: the dashboard between refreshes, the splash logo, and the
    // low-battery notice.
    //
    // A bench said the opposite (supply off, all lines high-Z, clean) but it slept twenty
    // seconds; these sleeps are minutes to hours. Short observation, wrong conclusion.
    //
    // Nothing is lost by keeping it: the controller has just been put in hibernate, where
    // its datasheet gives about a microamp — noise beside a floor of some three hundred.
    // The previous board revision had no switch at all and never showed this.
    // The experiment is over and it answered. The panel maker's 2-3 second settle between
    // the sleep command and power removal DOES run — the log shows partial ticks going
    // from 952 ms to 2981 ms, exactly the two seconds — and the glass greyed anyway, this
    // time within minutes rather than overnight.
    //
    // Getting worse rather than merely staying bad is itself the finding: 0.9.26 was the
    // first build to pair a CUT rail with a FAST cleaning frame, and their mail reserves
    // the full refresh for "thorough screen clearing during power on/off". A short
    // waveform does not drive pixels all the way to their rails; a powered controller
    // holds them there and an unpowered one cannot.
    //
    // So the supply stays on, as settled. If the switch is ever wanted again the pairing
    // to avoid is now known, and the settle code stays where it is for the parked screens.
    //
    // The night is the one stretch where that pairing can be had for free, and it is now taken:
    // the frame that starts the night is full by construction (plan_tick), and nothing is
    // repainted until morning, so there is no partial to protect. Eight hours of the twenty-four
    // with the rail down.
    //
    // The guard is not decoration. Parking over a partial is the one way this feature can ruin
    // the glass, and the caller decides to park several function calls before the frame is drawn.
    // If the two ever disagree, the honest outcome is a supply left on and a line in the log —
    // not a grey screen at dawn that nobody can explain.
    bool park_now = park;
    if (park_now && drew == 1) {
        ESP_LOGE(TAG, "refusing to cut the panel's supply over a partial frame — keeping it on");
        park_now = false;
    }
    panel_to_sleep(epd, park_now ? PanelSleep::Parked : PanelSleep::Powered);
    ESP_LOGW(TAG, "panel: %s, supply %s — sleeping %u s",
             drew == 2 ? "cleaning frame" : drew == 1 ? "partial" : "no repaint",
             park_now ? "off for the night" : "kept",
             (unsigned)sleep_s);

    // Backing off only while the line is misbehaving. The recorder counted 187 pin wakes
    // in eight minutes with the cable out; arming through that keeps the device awake a
    // third of the time and repaints the panel every few seconds. The timer still runs,
    // so a real cable change is noticed at the next scheduled wake instead of instantly.
    // The cable's level for the delay further down is taken NOW, before arming: usb_present()
    // reconfigures the pad with gpio_config(), and nothing touches a wake pin once its wake
    // has been enabled.
    const bool cable_at_sleep = usb_present();
    arm_wake_pins(rtc_usb_spurious >= USB_STORM ? UsbWake::None : UsbWake::OnChange);
    if (rtc_usb_spurious >= USB_STORM)
        EvtLog::getInstance().event(EVT_POWER, 0x20000u | rtc_usb_spurious);
    // The sleep length IS the profile (computed above). With a live clock the screen has to
    // change every minute, so the device wakes every minute — about 1440 times a day.
    // Without it there is nothing to show between readings, so it sleeps straight through
    // to the next one: about 288 wakes instead, and that gap is most of what Ultra Save
    // actually saves. Radio and display cost far less than simply being awake.
    // A press caught by the interrupt during this cycle turns the sleep into a blink: the
    // device goes down and comes straight back, and the tick that follows treats it as a
    // button wake. Reusing the ordinary path this way means the press does exactly what it
    // would have done had it arrived a second later, with no second implementation of what
    // a press means.
    // From here the button is guarded by the deep-sleep GPIO wake, not by this interrupt,
    // and the panel rail is about to be switched — the one event most likely to put a spike
    // on the line. An edge accepted now would survive into the next boot as a press that
    // never happened, so the handler is taken off the pin first.
    gpio_intr_disable(PIN_USER_BTN);
    if (s_btn_glitches)
        ESP_LOGW(TAG, "button line: %u edge(s) rejected as noise this cycle (%u total)",
                 (unsigned)s_btn_glitches, (unsigned)rtc_btn_noise);
    uint64_t timer_us = (uint64_t)sleep_s * 1000000ULL
                      + (uint64_t)(sleep_adj_ms > 0 ? sleep_adj_ms : 0) * 1000ULL;
    if (s_btn_pressed_while_awake) {
        s_btn_pressed_while_awake = false;
        rtc_btn_pending = true;
        // Carry the time already served. The classifier on the other side of this sleep
        // starts its own count at zero, and a press that began while the device was busy
        // has by then been held for seconds — a measurement wake alone is about six. The
        // user holds for three, the counter sees half a second, and a deliberate long press
        // is delivered as a short one. That is the "button does not work the first time"
        // this project has been writing off as "press it again" since 4.54.
        // Only counted while the finger is still DOWN: if it has already come off, the
        // press is over and its length is unknowable from falling edges alone, so claiming
        // a duration would be inventing one.
        rtc_btn_held_ms = 0;
        if (gpio_get_level(PIN_USER_BTN) == 0 && s_btn_press_us) {
            const int64_t el = (esp_timer_get_time() - s_btn_press_us) / 1000;
            if (el > 0 && el < 60000) rtc_btn_held_ms = (uint32_t)el;
        }
        ESP_LOGW(TAG, "a press arrived mid-cycle (%u ms served) — waking straight back up",
                 (unsigned)rtc_btn_held_ms);
        timer_us = 50000ULL;      // 50 ms
    }
    ESP_LOGW(TAG, "sleeping %u s (or until the button)", (unsigned)sleep_s);
    ESP_LOGW(TAG, "==================================================");
    // The log is flushed only when a monitor could actually be listening. On battery that is
    // 200 ms of full-speed awake time, some 1700 times a day, spent flushing a log to a port
    // that does not exist — about a tenth of the daily budget for nothing.
    enter_deep_sleep(timer_us, cable_at_sleep);
}

// This boot is a console session asked for from Matter mode (rtc_matter_console, read at the top).
static bool s_matter_console = false;

// ---- CONFIGURED: the working cycle -------------------------------------------------------------
// Gather everything first, draw once, then sleep. Drawing last is deliberate: the screen must
// never show half the story while the rest is still on its way.
[[noreturn]] static void run_working_cycle(EPaper &epd, const ButtonWake &bw)
{
    NVSManager::getInstance().init();

    // Light sleep, armed and immediately forbidden. It is lifted only inside the two sensor waits
    // (PmGate::Nap), which the flight recorder measured at 46 % of everything this device spends
    // awake. Everything else in the cycle — the panel, the radio, the deep sleep at the end —
    // behaves exactly as before, and that is deliberate: automatic light sleep everywhere is what
    // 0.9.43 removed after it stopped the button waking the device from the logo screen.
    PmGate::begin(PIN_USER_BTN, PIN_LED, note_button_from_nap);

    if (rtc_magic != CYC_MAGIC) start_cycle_from_scratch();   // after setup, after power loss, or a moved layout

    // Cheap pre-check of whether this wake will measure — the full decision needs the plan,
    // but the gauge read has to be decided before that, and both follow the same clock.
    const bool do_meas_soon = (rtc_tick == 0) || rtc_last_meas == 0 ||
                              time(NULL) < 1600000000 ||
                              (time(NULL) - rtc_last_meas) >= GAS_SEC - GAS_EARLY_SEC;

    // Before anything is spent: is there enough left to spend? Below the regulator's floor
    // the rail cannot be held, so the device stops rather than browns out mid-write. The
    // screen is painted once and then left alone — e-ink keeps it with no power at all,
    // which is exactly what this state needs.
    if (battery_too_low(do_meas_soon)) sleep_low_battery(epd);

    const Tick tick = plan_tick(bw);

    // The rail comes up HERE, after the decision above and not before it. init() is what switches
    // the panel on, and while it stood ahead of plan_tick() night parking cost more than it saved:
    // every night wake powered the rail, drew nothing, and then paid the vendor's two-second
    // settle and three seconds of quiet to take it down again. Measured on the device overnight,
    // 2026-09-22 — a parked night wake ran 9342 ms against 5265 ms for the same wake that DREW the
    // night before. The log said it in one line and no amount of reading the diff would have.
    if (!night_frozen(tick) && !epd.init()) ESP_LOGE(TAG, "EPaper init FAILED");

    // 1. sensors
    // EF_MEASURE was set only by the Matter loop, so in Wi-Fi mode every measuring wake was
    // filed as an ordinary tick: 52.7 h of log showed 12 measurements instead of 601, and the
    // ordinary tick carried a p90 of 6129 ms that was simply the measurements hiding inside it.
    // The night and forced flags were missing on this path for the same reason.
    EvtLog::getInstance().setFlags(
        ((tick.do_meas || tick.forced) ? EF_MEASURE : 0) |
        (tick.forced ? EF_FORCED : 0) |
        (tick.plan.night ? EF_NIGHT : 0));
    // CO2 has its own interval and it is NOT a multiple of the gas period, so it needs its own
    // place in this condition. Without it the reading could only ever happen on a gas tick, and the
    // profile's interval was silently rounded up to the next whole one: gas is due at 298 s while
    // CO2 wants 300, so a gas tick that lands a second or two short leaves CO2 not-yet-due, and the
    // next chance is a whole period later. Performance therefore measured every 10 minutes instead
    // of 5 — reported on the owner's device 2026-09-30, and the numbers are indistinguishable from
    // the night doubling, which is what made it look like a setting.
    //
    // The Matter loop was given exactly this fix on 2026-09-17, with a comment asserting the
    // working cycle already read CO2 on every tick. It did not: this gate has been do_meas-only
    // since the phase split in 0.9.47. A fix applied to one of two siblings on a belief about the
    // other is how this survived two weeks.
    if (tick.do_meas || tick.do_co2 || tick.forced) {
        measure(tick);
        note_light_sleep();          // the two gated waits just happened; say what they cost
    } else {
        follow_cable();              // a cable wake that measures nothing still tells the battery
    }

    // 2. internet: time first, then weather
    radio_window(tick);

    // Hand everything to the web console. It reads a separate set of variables, so
    // without this the dashboard reports zeros however fresh the readings are.
    publish_globals();

    // 3-5. the glass, and the console on a long press
    const Shown shown = draw_and_serve(epd, tick);

    // The console changed the city or the weather key (web_server.cpp): the next window
    // fetches the weather and forecast at once instead of waiting out the radio interval.
    if (global_weather_dirty) {
        global_weather_dirty = false;
        rtc_last_wx_try = 0; rtc_last_fc = 0; rtc_met_expires = 0;   // a new place is a new question
    }
    // Same shape for the hub: a new broker or a new topic means Home Assistant has never been told
    // what publishes there. Zeroing the stamp makes the next window re-announce instead of waiting
    // out the daily interval — which is what "after a settings change" in publish_to_hub() promised
    // and never did (the stamp is RTC_NOINIT, so "after a restart" was never true either).
    if (global_hub_dirty) {
        global_hub_dirty = false;
        rtc_mqtt_disco_at = 0;
        ESP_LOGW(TAG, "hub or topic changed — Home Assistant is told what this device is again");
    }

    // A console session asked for from Matter mode is over: back to Matter at once rather than at the
    // next tick, through a 50 ms sleep so the boot is an ordinary timer wake.
    if (s_matter_console) {
        rtc_tick++;
        led(false);
        panel_to_sleep(epd, PanelSleep::Powered);
        gpio_intr_disable(PIN_USER_BTN);
        s_btn_pressed_while_awake = false;
        const bool cable = usb_present();
        arm_wake_pins(UsbWake::None);
        ESP_LOGW(TAG, "console session over — %s", MatterMode::selected() ? "back to Matter" : "Wi-Fi mode from now on");
        enter_deep_sleep(50000ULL, cable);
    }

    const NextWake next = time_to_next_wake(tick, shown.full, shown.live_clock, 0, shown.drew);
    rtc_tick++;
    led(false);
    sleep_after_cycle(epd, next, shown.drew, tick.night_park);
}

// ---- NOT CONFIGURED: the logo, and nothing else until somebody presses the button ----------
// One set of rules whether the power comes from the cell or the cable: the logo goes on the glass,
// the panel loses its supply, and the device sleeps with the BUTTON as its only way back — no
// timer, no cable wake, no setup window opening on its own.
// The battery for the setup screen of a device that has not measured anything: one reading, taken
// here, before the radio comes up — a transmitting radio sags the cell. On battery, the figure from
// this board's own cell curve. On the cable the plug, with the bolt while the charger says it is
// charging and 100 % once it says it is done; the figure is left out while charging, because the
// voltage is then the charger's and this boot has nothing to tame it with (the dashboard's ceiling
// counts from the moment the cable went in, and nobody here saw that moment). v1 cannot say whether
// a charge is done without a rate this boot never read, so there the plug stands alone rather than
// claim current is flowing. False when the cell could not be read: the corner then stays empty.
static bool setup_battery(PortalScreen::Battery &b)
{
    b = PortalScreen::Battery{};
    b.usb = usb_present();
    I2CManager::getInstance().init();
    i2c_master_bus_handle_t bus = I2CManager::getInstance().getBusHandle();
    const BatterySource::Rev rev = BatterySource::revision(bus);
    if (b.usb) {
        if (rev == BatterySource::RevDividerV2) {
            if (BatterySource::chargeComplete(true) == 1) b.percent = 100;
            else                                          b.charging = true;
        }
        ESP_LOGW(TAG, "setup screen battery: on the cable, %s", b.percent == 100 ? "charge done"
                      : b.charging ? "charging" : "charge state unknown on this board");
        return true;
    }
    float v = 0.0f;
    if (!BatterySource::readVoltage(bus, v)) {
        ESP_LOGW(TAG, "setup screen battery: the cell could not be read — corner left empty");
        return false;
    }
    float pct = BatterySource::socFromVoltage(v, BatterySource::cellFor(rev));
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    b.percent = (int)pct;               // truncated, as the dashboard's (int)rtc_soc
    ESP_LOGW(TAG, "setup screen battery: %.3f V, %d %%", v, b.percent);
    return true;
}

[[noreturn]] static void run_unconfigured(EPaper &epd, bool cold, const ButtonWake &bw)
{
    const bool by_button = bw.by_button, btn_seen_down = bw.seen_down;
    if (cold || !by_button) {
        // Out of the box, after a power cycle, after a restart, or after a factory reset: show the
        // logo and wait to be asked. Nothing opens the setup window except somebody pressing the
        // button.
        //
        // The test used to be "cold" alone, and a factory reset looked warm: it ended in esp_restart(),
        // which leaves RTC memory standing, so the device fell through to the branch meant for a
        // button press and raised the access point on its own.
        NVSManager::getInstance().init();
        std::string ssid;
        NVSManager::getInstance().getString("ssid", ssid, "");
        ESP_LOGW(TAG, "%s | Wi-Fi %s — logo, then sleeping until the button",
                 cold ? "power-on or restart" : "woken, not by a press",
                 ssid.empty() ? "NOT configured" : ssid.c_str());
        // Only a wake out of this device's OWN sleep may trust that the logo is still on the glass.
        // After a power-on, a restart or a factory reset nobody can vouch for what is there, so it is
        // drawn. When it is trusted the panel is not touched at all: init() switches the supply on,
        // so a wake that decides nothing — a rejected phantom, the fallback timer — would otherwise
        // bring the rail up only to cut it again.
        if (!cold && rtc_logo_shown) ESP_LOGW(TAG, "logo already on the glass — panel left untouched");
        else if (!epd.init())        ESP_LOGE(TAG, "EPaper init FAILED — nothing will appear on the glass");
        else                         draw_rle(epd, "LOGO", logo4g_plane24_rle, LOGO4G_PLANE24_RLE_LEN,
                                      logo4g_plane26_rle, LOGO4G_PLANE26_RLE_LEN);
    } else {
        // The button was pressed: open the setup window.
        NVSManager::getInstance().init();
        std::string ssid;
        NVSManager::getInstance().getString("ssid", ssid, "");
        ESP_LOGW(TAG, "opening setup | Wi-Fi currently %s",
                 ssid.empty() ? "NOT configured" : ssid.c_str());

        // Picture first, radio second — the four-gray transfer needs DMA memory that Wi-Fi would
        // otherwise be holding, and when it is short the frame silently never reaches the panel.
        // A panel that will not come up does not cancel the window: the console works without it.
        if (epd.init()) {
            PortalScreen::Battery bat;
            draw_setup_screen(epd, "QR SETUP", setup_battery(bat) ? &bat : nullptr);
        } else {
            ESP_LOGE(TAG, "EPaper init FAILED — opening setup without the picture");
        }
        radio_up();
        // The password stays off the log: logs get pasted into bug reports.
        ESP_LOGW(TAG, "connect to \"%s\" with the password on the glass and open 192.168.4.1",
                 BENCH_AP_SSID);

        const bool visited = hold_setup_window(epd, nullptr);

        // Radio down BEFORE the next refresh — see radio_down() for why this order matters.
        radio_down();
        global_portal_remain = -1;

        // A window that a finger-less wake opened and nobody came to counts against the next one (see
        // the classifier). A press still held when it opened, or anybody using it, clears the count.
        if (visited || btn_seen_down) {
            if (rtc_portal_unused) ESP_LOGW(TAG, "setup window was used — unused-window count cleared");
            rtc_portal_unused = 0;
        } else if (rtc_portal_unused < 255) {
            rtc_portal_unused++;
            ESP_LOGW(TAG, "nobody used the setup window (%u in a row)", (unsigned)rtc_portal_unused);
        }

        NVSManager::getInstance().getString("ssid", ssid, "");
        uint8_t ok8 = 0; NVSManager::getInstance().getU8("wifi_ok", ok8, 0);
        // Set up during this window by any of the three routes — a joined network, no network, Matter —
        // whether or not the wizard reached its last step. A page closed before "Apply & restart" used
        // to leave such a device asleep on the logo until somebody pressed the button.
        const bool now_configured = device_configured();

        // Setup just finished: this is the moment the device arrives in the room it will live in.
        // Air quality is a RELATIVE measure — BSEC learns what "normal" smells like here over
        // about four days — so the calibration accumulated on our bench describes OUR workshop,
        // not the user's home. Carrying it over would have the device confidently rate a
        // stranger's room against the wrong baseline. Starting from an honest 0/3 is better.
        if (now_configured) {
            uint8_t seeded = 0;
            NVSManager::getInstance().getU8("cal_seeded", seeded, 0);
            if (!seeded) {
                BSECManager::invalidateRtcState();          // drop the in-RAM copy
                // ... and the flash backup behind it. This opened "storage" — the name of the raw
                // history PARTITION, not of the NVS namespace anything here is written under, which
                // is "smart_meteo". nvs_open() happily created an empty namespace by that name and
                // erased two keys that were never in it, reporting success, so the workshop's
                // calibration went out of the door intact: the device would judge the customer's
                // air against a baseline learned on our desk and show 3/3 confidence while doing it.
                // The whole point of 4.4 is that this index is relative to the room it learned.
                nvs_handle_t h;
                if (nvs_open("smart_meteo", NVS_READWRITE, &h) == ESP_OK) {
                    // All four, not two: the state blob is worthless without its clock.
                    static const char *kCal[] = { "bsec_state", "bsec_clk", "bsec_wall", "bsec_acc" };
                    for (const char *k : kCal) {
                        const esp_err_t e = nvs_erase_key(h, k);
                        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
                            ESP_LOGE(TAG, "could not erase %s: %s", k, esp_err_to_name(e));
                    }
                    nvs_commit(h); nvs_close(h);
                }
                rtc_bsec_nvs_at = 0; rtc_bsec_nvs_acc = 255;
                NVSManager::getInstance().setU8("cal_seeded", 1);
                ESP_LOGW(TAG, "setup complete — air-quality calibration reset "
                              "so it learns THIS room from scratch");
            }
        }
        ESP_LOGW(TAG, "setup closed | Wi-Fi %s | %s",
                 (!ssid.empty() && ok8) ? ssid.c_str()
                 : ssid.empty() ? "none" : "saved but never joined",
                 now_configured ? "set up — starting work" : "STILL not set up");
        // Straight into work: the next boot runs the measuring cycle or Matter and draws its own screen.
        if (s_window_poweroff) enter_power_off(epd);   // never returns
        if (now_configured || s_window_reboot) restart_from_window(epd);
        if (epd.init()) draw_rle(epd, "LOGO", logo4g_plane24_rle, LOGO4G_PLANE24_RLE_LEN,
                                      logo4g_plane26_rle, LOGO4G_PLANE26_RLE_LEN);
    }

    // ---- into the sleep that only the button ends ----------------------------------------------
    // PARKED: the supply is cut. This is the case the switch is on the board for — a finished picture
    // and a sleep that may last months in a box, one crossing for all of it. The controller gets its
    // sleep command and the vendor's settle first, with every pad still driven, then the lines go to
    // high impedance and the gate is latched off (panel_to_sleep, holdOffForDeepSleep).
    //
    // (0.9.42 briefly kept the supply on here, on the theory that an unpowered panel drags the button
    // line down. The button has its own 10k pull-up to 3.3 V, R10; that build was never released.)
    panel_to_sleep(epd, PanelSleep::Parked);
    ESP_LOGW(TAG, "panel parked: supply CUT — the picture stays on the glass without power");

    // A press made while the logo was going up or the panel was being put away is a press. The button
    // interrupt stays on through all of it (btn_isr already refuses an edge whose line is not low),
    // and whatever it caught is honoured the way the running cycle honours one: sleep for 50 ms and
    // let the next boot open the setup window. 0.9.40 and 0.9.41 switched the interrupt off before the
    // park and threw the flag away, so the first press after the logo appeared did nothing at all.
    gpio_intr_disable(PIN_USER_BTN);
    const bool press_during_park = s_btn_pressed_while_awake;
    s_btn_pressed_while_awake = false;
    rtc_btn_held_ms = 0;

    // A finger on the button right now would leave it unarmed — arming the level the line is already
    // at wakes the device instantly — and this sleep has no timer, so an ordinary press made at the
    // wrong moment would stall the button for the fifteen-minute fallback below. Wait for it to come
    // up first. Bounded: a jammed switch still falls through to the fallback.
    wait_button_release("going to sleep");
    rtc_btn_pending = press_during_park;
    if (press_during_park) {
        vTaskDelay(pdMS_TO_TICKS(60));            // the release bounces; it belongs to this press
        s_btn_pressed_while_awake = false;
        ESP_LOGW(TAG, "a press arrived while the logo was going up — waking straight back into setup");
    }

    // Wake on the BUTTON only. Nothing else can end this sleep, so arming it is the whole difference
    // between "waiting" and "gone". arm_wake_pins() disables every other source first, so there is no
    // timer unless one is set just below.
    const bool armed = arm_wake_pins(UsbWake::None);
    uint64_t timer_us = 0;
    if (press_during_park) {
        timer_us = 50000ULL;                      // 50 ms: straight back into setup
    } else if (!armed) {
        // Not armed: still at ground after the wait (jammed or shorted), or the line has produced too
        // many phantoms in a row. Arming it anyway would wake the device the instant it slept, for
        // ever; leaving it unarmed with no timer would strand it. Come back in fifteen minutes.
        ESP_LOGW(TAG, "button not armed — waking in 15 min to try again instead of sleeping for ever");
        timer_us = 900ULL * 1000000ULL;
    }

    led(false);                       // the work is done: the light goes out as the device does
    ESP_LOGW(TAG, "sleeping until the BUTTON is pressed");
    ESP_LOGW(TAG, "==================================================");
    enter_deep_sleep(timer_us, true);
}

// ---- MATTER MODE: one boot that never deep sleeps --------------------------------------------------
// A Thread end device has to stay in its network, so the working cycle runs here as a loop inside one
// boot: the same phases as the Wi-Fi cycle (plan, measure, the glass, the next tick), with automatic
// light sleep between ticks instead of deep sleep (owner, 2026-09-16), and the readings published to
// the smart home after each measurement. What the Wi-Fi mode gets from the internet does not exist
// here: no weather, no Telegram, no MQTT, no scheduled updates. A long press restarts into one Wi-Fi
// console session (clock and updates over the home network, or the device's own access point) and
// then comes back.

// Ends the wait between ticks early: a press, or anything the Matter stack reports.
static SemaphoreHandle_t s_loop_wake = nullptr;
static void matter_poke(void)
{
    if (s_loop_wake) xSemaphoreGive(s_loop_wake);
}

// The light-sleep wake for the button is a LEVEL (sleep_modes.rst, "GPIO Wakeup": gpio_wakeup_enable()
// + esp_sleep_enable_gpio_wakeup()), and gpio_wakeup_enable() gives the pin's interrupt that same low
// level. A level interrupt keeps firing while the finger is down, so the handler switches itself off
// and the cycle switches it back on once the press is over — the pattern esp-iot-solution's button
// component uses in its power-save mode.
static void matter_btn_isr(void *)
{
    gpio_intr_disable(PIN_USER_BTN);
    if (!s_btn_pressed_while_awake) s_btn_press_us = esp_timer_get_time();
    s_btn_pressed_while_awake = true;
    BaseType_t woken = pdFALSE;
    if (s_loop_wake) xSemaphoreGiveFromISR(s_loop_wake, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static bool s_btn_stuck = false;   // held past wait_button_release(): not armed until it is seen up

static void matter_arm_button(void)
{
    if (s_btn_stuck) {
        if (gpio_get_level(PIN_USER_BTN) == 0) return;
        s_btn_stuck = false;
        ESP_LOGW(TAG, "button released — armed again");
    }
    gpio_wakeup_enable(PIN_USER_BTN, GPIO_INTR_LOW_LEVEL);
    gpio_intr_enable(PIN_USER_BTN);
}

// The cable, the same way: a light-sleep wake on the level the detect line is NOT at, so plugging or
// pulling the cable ends the wait and the plug icon follows at once instead of at the next tick (up to
// five minutes without a clock). The deep-sleep path arms the opposite level too (arm_wake_pins,
// OnChange). usb_present() reconfigures the pad on every read, which takes this off again, so the loop
// arms it right before each wait. A line that keeps waking the loop without the cable changing is
// noise; after USB_STORM of those it is left unarmed until a tick ends on its own timer.
static volatile bool s_usb_woke = false;
static int     s_usb_armed_level = -1;
static uint8_t s_usb_noise = 0;

static void matter_usb_isr(void *)
{
    gpio_intr_disable(PIN_USB_DETECT);
    s_usb_woke = true;
    BaseType_t woken = pdFALSE;
    if (s_loop_wake) xSemaphoreGiveFromISR(s_loop_wake, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void matter_arm_usb(void)
{
    // Fired while the tick was busy: that tick (or the one the semaphore now triggers) draws the new
    // state, so it is a change, not noise — the level read below is already the new one.
    if (s_usb_woke) { s_usb_woke = false; s_usb_noise = 0; }
    if (s_usb_noise >= USB_STORM) {
        gpio_wakeup_disable(PIN_USB_DETECT);   // the light-sleep wake too, not only the interrupt
        s_usb_armed_level = -1;
        return;
    }
    const int level = gpio_get_level(PIN_USB_DETECT);
    s_usb_armed_level = level;
    gpio_wakeup_enable(PIN_USB_DETECT, level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    gpio_intr_enable(PIN_USB_DETECT);
}

// Before any deep sleep taken from this mode (the low-battery stop): the light-sleep wakes and the
// level interrupts go, and arm_wake_pins() takes it from there.
static void matter_button_off(void)
{
    gpio_intr_disable(PIN_USER_BTN);
    gpio_wakeup_disable(PIN_USER_BTN);
    gpio_intr_disable(PIN_USB_DETECT);
    gpio_wakeup_disable(PIN_USB_DETECT);
}

// A press the interrupt caught, classified the way classify_button_wake() does it for a deep-sleep
// wake: a finger must be seen within 300 ms, three seconds held is a long press, the release and its
// bounce belong to this press. The held time runs from the edge the interrupt saw.
static ButtonWake matter_take_press(void)
{
    ButtonWake bw{false, false, false, 0};
    if (!s_btn_pressed_while_awake) return bw;
    // A press caught while the tick was busy (a gas sample, a refresh, the stack starting) is handled
    // late, often after the finger is gone. The interrupt only fires on a low line, so such a press is
    // honoured as a short one — what rtc_btn_pending does on the sleeping path. Only a fresh edge must
    // still show a finger within 300 ms.
    const bool fresh = (esp_timer_get_time() - s_btn_press_us) < 300000;
    bool down = false;
    for (int i = 0; i < 30 && !down; ++i) {
        if (gpio_get_level(PIN_USER_BTN) == 0) down = true;
        else if (!fresh) break;
        else vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!down && fresh) {
        rtc_btn_noise++;
        s_btn_pressed_while_awake = false;
        ESP_LOGW(TAG, "button interrupt with nothing held — ignored (%u so far)", (unsigned)rtc_btn_noise);
        matter_arm_button();
        return bw;
    }
    if (!down) ESP_LOGW(TAG, "press made while the tick was busy — handled now");
    led(true);
    int held = 0;
    while (gpio_get_level(PIN_USER_BTN) == 0) {
        held = (int)((esp_timer_get_time() - s_btn_press_us) / 1000);
        if (held >= 3000) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (held < 0) held = 0;
    const bool longp = held >= 3000;
    if (longp) {
        for (int i = 0; i < 3; i++) {
            led(false); vTaskDelay(pdMS_TO_TICKS(150));
            led(true);  vTaskDelay(pdMS_TO_TICKS(150));
        }
        wait_button_release("long press");
    }
    wait_button_release("press");
    // Still down after wait_button_release() gave up: a jammed switch, not a person. A level interrupt
    // re-armed on a low line fires at once, and every such "press" held three seconds became a long one
    // — a restart into the console, over and over. The deep-sleep path refuses a held button the same
    // way (arm_wake_pins); the loop arms it again once the line is seen up.
    if (gpio_get_level(PIN_USER_BTN) == 0) {
        s_btn_pressed_while_awake = false;
        s_btn_stuck = true;
        led(false);
        ESP_LOGW(TAG, "button held for more than 10 s — ignored until it is released");
        EvtLog::getInstance().event(EVT_BUTTON, 0x40000u | (uint32_t)(held & 0xFFFF));
        return bw;
    }
    vTaskDelay(pdMS_TO_TICKS(60));
    s_btn_pressed_while_awake = false;
    ESP_LOGW(TAG, "button: %s press (%d ms)", longp ? "LONG" : "SHORT", held);
    EvtLog::getInstance().event(EVT_BUTTON, (uint32_t)(held & 0xFFFF) | (longp ? 0x10000u : 0u));
    matter_arm_button();
    return ButtonWake{true, longp, false, true, 0};
}

// The smart-home verdict: the worse of the gas class and the CO2 level. Matter's air quality has six
// levels; Bosch's seven classes map onto them with Excellent and Good both "good". CO2 levels follow
// the device's own default alert (1100 ppm) for "moderate". Unknown while neither sensor has a verdict.
static int matter_air_quality(void)
{
    int lvl = 0;
    if (rtc_co2 > 0 && rtc_co2_fails < 3)
        lvl = rtc_co2 < 800 ? 1 : rtc_co2 < 1100 ? 2 : rtc_co2 < 1500 ? 3
            : rtc_co2 < 2000 ? 4 : rtc_co2 < 5000 ? 5 : 6;
    if (rtc_acc >= 1 && rtc_cls_txt[0]) {
        static const int8_t MAP[7] = { 1, 1, 2, 3, 4, 5, 6 };
        const int g = MAP[(int)Analytics::iaqBand(rtc_iaq) % 7];
        if (g > lvl) lvl = g;
    }
    return lvl;
}

static void matter_publish(void)
{
    MatterMode::Readings r{};
    const bool scd_ok = rtc_co2_fails < 3;
    r.co2_valid     = scd_ok && rtc_co2 > 0;
    r.co2           = rtc_co2;
    r.climate_valid = scd_ok && rtc_temp != 0.0f;
    r.temp_c        = rtc_temp;
    r.hum           = rtc_hum;
    r.air_quality   = matter_air_quality();
    MatterMode::publish(r);
}

// The pairing screen. The code comes from NVS and the QR is built here; the glass is left asleep, and
// the dashboard's baseline is void once this picture is on it.
static void draw_pairing(EPaper &epd, int state)
{
    MatterMode::Codes c;
    static uint8_t grid[29 * 29];
    int n = 0;
    // True random only with a radio running; otherwise codes() switches the internal entropy source on.
    const bool have = MatterMode::codes(c, MatterMode::running());
    if (have) n = MatterMode::qr_grid(c.qr, grid, sizeof(grid));
    PairingData d{};
    d.qr = n ? grid : nullptr;
    d.qrSize = n;
    d.code = have ? c.manual_fmt : nullptr;
    d.state = state;
    rtc_prev_valid = false; rtc_prev = Frame{};
    rtc_logo_shown = false;
    const int64_t t0 = esp_timer_get_time();
    if (!epd.init()) { ESP_LOGE(TAG, "EPaper init FAILED — no pairing screen"); return; }
    drawPairingScreen(epd, d);
    epd.refresh(EPaper::Update::Full);
    const bool ok = epd.sessionOk();
    epd.hibernate();
    ESP_LOGW(TAG, "pairing screen (%s) drawn in %lld ms%s | QR %d modules",
             state == PAIR_OPEN ? "open" : state == PAIR_JOINING ? "connecting"
             : state == PAIR_CLOSED ? "paused" : "error",
             (long long)((esp_timer_get_time() - t0) / 1000), ok ? "" : " — PANEL FAULT", n);
}

[[noreturn]] static void run_matter_mode(EPaper &epd, ButtonWake bw)
{
    NVSManager::getInstance().init();
    if (rtc_magic != CYC_MAGIC) start_cycle_from_scratch();
    rtc_wifi_fail = false;            // no Wi-Fi here: the pill would report a network nobody uses

    if (battery_too_low(true)) sleep_low_battery(epd);
    if (!epd.init()) ESP_LOGE(TAG, "EPaper init FAILED");

    // The pads that must hold between frames: panel, its power gate, the sensor bus, the light.
    EPaper::keepPadsThroughLightSleep();
    I2CManager::keepPadsThroughLightSleep();
    gpio_sleep_sel_dis(PIN_LED);

    if (!s_loop_wake) s_loop_wake = xSemaphoreCreateBinary();
    MatterMode::set_event_hook(matter_poke);

    const bool forget = MatterMode::forget_requested();
    const bool up = MatterMode::start();
    if (!up) EvtLog::getInstance().event(EVT_NOTE, 0x4D540001u);   // 'MT' 1: the stack did not start
    if (up && forget) {
        ESP_LOGW(TAG, "the console asked to forget the smart home");
        MatterMode::clear_forget();          // carried out now; a stack that did not start kept it
        panel_to_sleep(epd, PanelSleep::Powered);
        rtc_boot_incomplete = 0;
        MatterMode::forget_and_restart();
    }

    // Light sleep from here on, and the button as a light-sleep wake.
    configure_power_management(true);
    esp_pm_lock_handle_t awake = nullptr;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "tick", &awake) != ESP_OK) awake = nullptr;
    gpio_isr_handler_remove(PIN_USER_BTN);
    gpio_isr_handler_add(PIN_USER_BTN, matter_btn_isr, nullptr);
    gpio_isr_handler_add(PIN_USB_DETECT, matter_usb_isr, nullptr);
    esp_sleep_enable_gpio_wakeup();
    matter_arm_button();

    int pair_shown = -2;              // what the pairing screen shows; -2 = the dashboard (or nothing yet)
    bool published = false;
    int64_t last_gas_mono = 0;        // monotonic stamp of the last gas sample, for a device with no clock
    int64_t last_co2_mono = 0;        // ...and of the last CO2 reading
    time_t  seen_wall = time(NULL);
    int64_t seen_mono = esp_timer_get_time();
    bool pressed = bw.by_button;

    for (;;) {
        if (awake) esp_pm_lock_acquire(awake);
        const int64_t t0 = esp_timer_get_time();
        esp_task_wdt_reset();
        if (!pressed) { bw = matter_take_press(); pressed = bw.by_button; }

        // The clock can only come from the hub (Time Synchronization) or from a console session.
        // Say so when it appears or moves, because nothing else in the log would.
        {
            const time_t wall = time(NULL);
            const int64_t mono = esp_timer_get_time();
            const long long expect = (long long)seen_wall + (mono - seen_mono) / 1000000LL;
            if (wall > 1600000000 && seen_wall <= 1600000000) {
                ESP_LOGW(TAG, "clock set while in Matter mode (by the hub) — %lld", (long long)wall);
                // Without a clock the stamps were kept by the monotonic timer; carry them over, or the
                // first tick with a clock finds rtc_last_meas zero and samples the gas sensor off its beat.
                if (last_gas_mono) rtc_last_meas = wall - (time_t)((mono - last_gas_mono) / 1000000LL);
                if (last_co2_mono) rtc_last_co2  = wall - (time_t)((mono - last_co2_mono) / 1000000LL);
            }
            else if (wall > 1600000000 && llabs((long long)wall - expect) > 120)
                ESP_LOGW(TAG, "clock moved by %+lld s while in Matter mode", (long long)wall - expect);
            seen_wall = wall; seen_mono = mono;
        }
        drop_impossible_stamps();

        const uint32_t ev = MatterMode::take_events();
        if (ev & MatterMode::EV_LAST_FABRIC) {
            // Removed from the last smart home: back to a device waiting to be added. Bluetooth was
            // released after commissioning (CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING), so only a restart
            // can advertise again, and esp_matter::factory_reset() is that restart.
            panel_to_sleep(epd, PanelSleep::Powered);
            rtc_boot_incomplete = 0;
            led(false);
            MatterMode::forget_and_restart();
        }

        if (bw.btn_long) {
            ESP_LOGW(TAG, "long press — restarting into a console session, Matter comes back after it");
            rtc_matter_console = MATTER_CONSOLE_MAGIC;
            matter_button_off();
            panel_to_sleep(epd, PanelSleep::Powered);
            rtc_boot_incomplete = 0;
            esp_restart();
        }

        const uint8_t fab = MatterMode::fabrics();
        if (up && fab == 0 && bw.by_button && !MatterMode::window_open()) {
            ESP_LOGW(TAG, "press while unpaired — reopening the pairing window");
            MatterMode::open_window();
        }

        Tick tick = plan_tick(bw);
        if (!tick.clock_ok) {
            // No clock, no schedule in seconds: the gas period is kept by the monotonic timer, which
            // in this mode (one long boot) is exactly the thing that is missing on the sleeping path.
            tick.do_meas = (last_gas_mono == 0) ||
                           (t0 - last_gas_mono) >= (int64_t)(GAS_SEC - GAS_EARLY_SEC) * 1000000LL;
        }
        // CO2 on its own schedule too, the same condition the working cycle uses. Reading only on gas
        // ticks rounded the CO2 period up to the next one: three gas periods land a second or two short
        // of fifteen minutes about half the time, and the reading then waited for the fourth.
        // (Until 2026-09-30 this comment claimed the working cycle read on every tick. It did not —
        // and that untrue claim is why the same defect sat unfixed in the sibling for two weeks.)
        const bool may_read = tick.do_meas || tick.do_co2 || tick.forced;
        if (battery_too_low(may_read)) { matter_button_off(); sleep_low_battery(epd); }

        if (!may_read) follow_cable();   // the plug icon follows the cable at once; the figure too
        if (may_read) {
            measure(tick);
            if (tick.do_meas) last_gas_mono = t0;
            if ((tick.do_co2 || tick.forced) && rtc_co2_fails == 0) last_co2_mono = t0;
            matter_publish();
            published = true;
        } else if (!published && up) {
            matter_publish();          // the readings this device already had, as soon as the stack is up
            published = true;
        }
        radio_window(tick);            // no radio in this mode: owed events are closed locally
        publish_globals();

        // The glass: the pairing screen until the device is in a smart home, the dashboard after.
        bool full_now = tick.due_full || tick.forced;
        int drew = 0;
        if (fab == 0) {
            const int st = !up ? PAIR_ERROR
                         : MatterMode::session_active() ? PAIR_JOINING
                         : MatterMode::window_open() ? PAIR_OPEN : PAIR_CLOSED;
            if (st != pair_shown || tick.forced) {
                draw_pairing(epd, st);
                pair_shown = st;
                drew = 2;
            }
        } else {
            if (pair_shown != -2) { full_now = true; pair_shown = -2; }
            if (ev & MatterMode::EV_PAIRED) full_now = true;
            drew = draw_dashboard(epd, full_now, tick.plan.night, true, tick.plan.live_clock);
        }

        // What the sleeping path does at its sleep: a pending image is judged, the tick is recorded,
        // and the boot counts as one that works.
        settle_pending_image();
        {
            EvtLog &evt = EvtLog::getInstance();
            evt.setWake(pressed ? WK_BUTTON : WK_TIMER, false);
            evt.setFlags((usb_present() ? EF_USB : 0) | (tick.plan.night ? EF_NIGHT : 0) |
                         (may_read ? EF_MEASURE : 0) | (tick.forced ? EF_FORCED : 0),
                         EF_USB | EF_NIGHT | EF_MEASURE | EF_FORCED);
            evt.setDetail(rtc_btn_noise);
            const int64_t ms = (esp_timer_get_time() - t0) / 1000;
            evt.wake((uint16_t)(ms < 1 ? 1 : ms > 65535 ? 65535 : ms));
            evt.setFlags(0, EF_DREW | EF_FULL | EF_FIRST | EF_RADIO);
        }
        rtc_boot_incomplete = 0;

        const NextWake next = time_to_next_wake(tick, full_now, tick.plan.live_clock, t0);
        rtc_tick++;
        led(false);

        int64_t wait_ms = (int64_t)next.s * 1000 + (next.adj_ms > 0 ? next.adj_ms : 0);
        if (!tick.clock_ok && last_gas_mono) {
            wait_ms = (int64_t)GAS_SEC * 1000 - (esp_timer_get_time() - last_gas_mono) / 1000;
            if (wait_ms < 5000) wait_ms = 5000;
        }
        ESP_LOGW(TAG, "tick done (%s) — light sleep up to %lld s | free heap %u",
                 drew == 2 ? "full refresh" : drew == 1 ? "partial" : "no repaint",
                 (long long)(wait_ms / 1000), (unsigned)esp_get_free_heap_size());

        // The wait. The watchdog is fed every 20 s; a press or a stack event ends it at once.
        bw = ButtonWake{false, false, false, false, 0};
        pressed = false;
        const int64_t until = esp_timer_get_time() + wait_ms * 1000;
        if (s_btn_stuck) matter_arm_button();
        matter_arm_usb();
        if (awake) esp_pm_lock_release(awake);
        bool woken_early = false;
        for (;;) {
            const int64_t left = (until - esp_timer_get_time()) / 1000;
            if (left <= 0) break;
            esp_task_wdt_reset();
            const TickType_t chunk = pdMS_TO_TICKS(left > 20000 ? 20000 : left);
            if (xSemaphoreTake(s_loop_wake, chunk ? chunk : 1) == pdTRUE) { woken_early = true; break; }
        }
        if (s_usb_woke) {
            s_usb_woke = false;
            const bool changed = gpio_get_level(PIN_USB_DETECT) != s_usb_armed_level;
            if (changed) s_usb_noise = 0;
            else if (s_usb_noise < 255 && ++s_usb_noise == USB_STORM)
                ESP_LOGW(TAG, "USB detect line woke the loop %u times without the cable changing — "
                              "not arming it until a tick ends on its timer", (unsigned)s_usb_noise);
            ESP_LOGW(TAG, "woken by the cable line (%s)", changed ? "cable changed" : "no change");
        } else if (!woken_early) {
            s_usb_noise = 0;
        }
    }
}

// ---- switching the device off by hand ---------------------------------------------------------
// This is NOT a new power state. It is a door into one the device has had since 0.9.43 and has run
// in ever since: the sleep an unconfigured device takes under its logo — sensors parked, the panel's
// supply cut, every wake source cleared and only the button armed, no timer at all. About nineteen
// microamps, and proven on hardware for months. Building a second such state would mean proving it
// again; giving the owner a way into this one does not.
//
// What "off" keeps: readings, settings, the gas calibration in NVS, the CO2 correction inside the
// SCD41's own EEPROM, and the clock, which runs from the crystal through deep sleep. Nothing is
// erased — that is the whole difference from a factory reset, which takes the same road out.
static bool power_off_pending(void)
{
    uint8_t off = 0;
    NVSManager::getInstance().init();
    NVSManager::getInstance().getU8("pwr_off", off, 0);
    return off != 0;
}

// The name the two screens carry, so a shelf with two devices on it is not a puzzle.
static const char *device_name_for_screen(char *buf, size_t n)
{
    std::string name;
    NVSManager::getInstance().init();
    NVSManager::getInstance().getString("dev_name", name, "");
    if (name.empty()) return nullptr;
    snprintf(buf, n, "%s", name.c_str());
    return buf;
}

[[noreturn]] static void enter_power_off(EPaper &epd)
{
    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, "switching off — readings and settings are kept");
    // Written BEFORE anything else can go wrong. If the draw or the park fails, the device still
    // comes back knowing it was switched off; the alternative is a device that quietly starts
    // measuring again because the one byte that remembers never got written.
    NVSManager::getInstance().init();
    NVSManager::getInstance().setU8("pwr_off", 1);

    EvtLog::getInstance().event(EVT_NOTE, NOTE_POWER_OFF);
    // Everything the console left standing, taken down in the same order the factory reset uses.
    // Doing it here rather than at each caller means every road into this function is safe, and
    // there are five of them: the ten-second press and four ways a setup window can close.
    vTaskDelay(pdMS_TO_TICKS(800));            // let the page's reply actually leave
    CaptiveDNS::getInstance().stop();
    WebServer::getInstance().stop();
    WiFiManager::getInstance().shutdown();
    park_sensors();

    char nb[40];
    const char *name = device_name_for_screen(nb, sizeof(nb));
    if (epd.init()) {
        const int64_t t0 = esp_timer_get_time();
        epd.clear(0xFF);
        drawOffScreen(epd, name);
        epd.refresh(EPaper::Update::Full);
        ESP_LOGW(TAG, "OFF screen drawn in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    }
    // Parked, not Powered: the picture was written with a full refresh, which is the one kind that
    // survives losing the supply (sleep_after_cycle spells out why), and an off device has no
    // business holding a rail up for months.
    panel_to_sleep(epd, PanelSleep::Parked);

    gpio_intr_disable(PIN_USER_BTN);
    s_btn_pressed_while_awake = false;
    // The finger is still on the button — it has been for ten seconds. Arming a wake source on a
    // line that is at ground wakes the device the instant it sleeps.
    wait_button_release("switching off");

    const bool armed = arm_wake_pins(UsbWake::None);
    uint64_t timer_us = 0;
    if (!armed) {
        // Same fallback the logo sleep has: a line that cannot be armed would strand the device
        // for ever, so come back in a quarter of an hour and try again rather than disappear.
        ESP_LOGW(TAG, "button not armed — waking in 15 min rather than sleeping for ever");
        timer_us = 900ULL * 1000000ULL;
    }
    led(false);
    ESP_LOGW(TAG, "OFF — sleeping until the BUTTON is pressed");
    ESP_LOGW(TAG, "==================================================");
    enter_deep_sleep(timer_us, true);
}

// The question on the glass, for the path where nobody has a phone in their hand. Everything about
// it is reversible: a second press confirms, and doing nothing at all puts the device back to work.
static bool confirm_power_off(EPaper &epd)
{
    char nb[40];
    const char *name = device_name_for_screen(nb, sizeof(nb));
    if (!epd.init()) { ESP_LOGE(TAG, "no panel — not asking about switching off"); return false; }
    epd.clear(0xFF);
    drawOffConfirmScreen(epd, name);
    epd.refresh(EPaper::Update::Full);
    ESP_LOGW(TAG, "asking about switching off — press again within %d s to confirm", OFF_CONFIRM_S);

    // A press, not a level: the button is released by now (the classifier waited for it), so what
    // we are looking for is a NEW edge. Two low readings 50 ms apart, the same debounce the setup
    // window uses, because one reading is what turned a bounce into a press before.
    gpio_intr_disable(PIN_USER_BTN);
    s_btn_pressed_while_awake = false;
    for (int elapsed = 0; elapsed < OFF_CONFIRM_S * 1000; elapsed += 50) {
        if (gpio_get_level(PIN_USER_BTN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(PIN_USER_BTN) == 0) {
                ESP_LOGW(TAG, "confirmed");
                return true;
            }
            elapsed += 50;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        if ((elapsed % 2000) == 0) esp_task_wdt_reset();
    }
    ESP_LOGW(TAG, "nobody confirmed — carrying on");
    return false;
}

void bench_screens_run(void)
{
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const bool cold = (cause == ESP_SLEEP_WAKEUP_UNDEFINED);

    const uint8_t boot_fails = count_unfinished_boot();

    // Watch this task. Everything the device does happens on it, so if it stops making progress
    // the device is simply gone — asleep-looking but never waking, which in the field is
    // indistinguishable from a dead battery until someone picks it up.
    //
    // Sixty seconds, because that has to clear the longest legitimate block, not the typical one:
    // fetching the weather over HTTPS can take twenty, the sensor's self test ten and a half, a
    // four-gray refresh seven. A timeout tight enough to catch a hang quickly would reboot the
    // device every time the weather server was slow, which is worse than the fault it prevents.
    // It panics rather than warns, so the reboot leaves a core dump behind to read afterwards.
    // Subscribed before the boot-failure branch: safe mode and its setup window feed this watchdog,
    // and feeding it from a task that is not subscribed only logs an error (wdts.rst).
    esp_task_wdt_add(NULL);

    // The panel's restart hook, bound to this task. The panel it acts on is named below, once there is one.
    s_restart_owner = xTaskGetCurrentTaskHandle();
    esp_register_shutdown_handler(panel_before_restart);

    if (boot_fails >= BOOT_FAIL_REVERT) recover_from_failed_boots(boot_fails);   // never returns

    configure_power_management(false);   // Matter mode switches light sleep on once its stack is up
    setup_led_and_button();

    // Ask the chip which pin woke it, rather than reading the level: a short press is long over
    // by the time boot gets here, so the level would say "not the button".
    const uint64_t wake_pins = esp_sleep_get_gpio_wakeup_status();
    const ButtonWake bw = classify_button_wake(cold, wake_pins);

    log_reset_reason();
    const bool by_usb = (wake_pins & (1ULL << PIN_USB_DETECT)) != 0;
    ESP_LOGW(TAG, "woken by %s | free heap %u",
             cold ? "POWER-ON" : bw.by_button ? "BUTTON" : by_usb ? "USB CABLE" : "timer",
             (unsigned)esp_get_free_heap_size());

    open_flight_recorder(cause, cold, by_usb, boot_fails, bw);

    if (rtc_build_stamp != buildStamp()) forget_previous_build();
    drop_impossible_stamps();

    EPaper epd;
    s_restart_panel = &epd;
    // The pads were latched before the last sleep so the panel's power FET could not float.
    // Release them first: a held pad ignores every later write without reporting anything.
    // Whether the rail comes back up is the previous sleep's decision, not this wake's: a parked
    // device that wakes only to check for a charger must not power the panel to do it. Any path
    // that actually draws calls init(), which switches the rail on itself.
    EPaper::releaseDeepSleepHold(rtc_panel_powered);

    // ---- a factory reset asked for by the console ----------------------------------------------
    // Before anything below reads a single setting: until this runs the device is still configured,
    // and the working cycle would carry on with the old owner's network, readings and history.
    {
        uint8_t fr = 0;
        NVSManager::getInstance().init();
        NVSManager::getInstance().getU8("fr_pending", fr, 0);
        if (fr || rtc_factory_req == FACTORY_REQ_MAGIC) factory_wipe();
    }

    // ---- switched off by hand ------------------------------------------------------------------
    // Two questions in one place, and the order matters. First: is the device supposed to be off
    // right now? Then: has somebody just asked for it to be?
    if (power_off_pending()) {
        if (bw.by_button) {
            // Somebody pressed it. Start for real — and start properly: rtc_tick = 0 makes this a
            // "first tick", which forces a full refresh, a measurement and a radio window all in
            // one go. The owner is standing in front of the device waiting for it to come back;
            // waiting out the ordinary schedule would look like it had not.
            NVSManager::getInstance().setU8("pwr_off", 0);
            rtc_tick = 0;
            ESP_LOGW(TAG, "button pressed while switched off — starting up");
        } else {
            // Not the button, so nothing has changed: this is the fifteen-minute fallback wake that
            // arm_wake_pins() falls back to when the line could not be armed. Go straight back
            // without touching the panel — the OFF screen is already on the glass and its supply
            // is off, and waking it just to redraw the same picture is exactly the cost this state
            // exists to avoid.
            ESP_LOGW(TAG, "still switched off (wake was not the button) — back to sleep");
            const bool armed = arm_wake_pins(UsbWake::None);
            enter_deep_sleep(armed ? 0 : 900ULL * 1000000ULL, true);
        }
    } else if (bw.btn_off) {
        // Ten seconds of holding. Ask, and only then do it.
        if (confirm_power_off(epd)) enter_power_off(epd);   // never returns
        // Declined, or nobody answered: fall through and let this wake carry on as an ordinary
        // long press. The console the owner half-asked for opens, which is the least surprising
        // thing that can happen after a press that was not confirmed.
    }

    // One definition of "set up", the same one the web server's setup cookie uses. It used to be
    // rebuilt here from ssid + wifi_ok, with no_wifi forcing only the second half, so a device set
    // up to run WITHOUT a network — the wizard saves just no_wifi=1 and no ssid — still read as
    // unconfigured: after "Apply & restart" it drew the logo and went back to sleep for good.
    // Read once: the unconfigured branch below ends in deep sleep, so nothing changes it in between.
    NVSManager::getInstance().init();
    const bool configured = device_configured();

    // The access point's own password, while nothing in this boot has started a radio yet — after a
    // factory reset (just above) this is where the new device draws its new one.
    WiFiManager::prepareApPassword();

    if (!configured) prepare_unconfigured();

    if (configured && MatterMode::selected()) {
        if (rtc_matter_console == MATTER_CONSOLE_MAGIC) {
            // Held for the console while in Matter mode: this boot is that console, run as a long press
            // of the Wi-Fi cycle, and it returns to Matter at its end (run_working_cycle).
            rtc_matter_console = 0;
            s_matter_console = true;
            ESP_LOGW(TAG, "Matter mode — console session");
            led(true);
            run_working_cycle(epd, ButtonWake{true, true, false, true, 0});   // never returns
        }
        run_matter_mode(epd, bw);                 // never returns
    }
    rtc_matter_console = 0;
    if (configured) run_working_cycle(epd, bw);   // never returns
    run_unconfigured(epd, cold, bw);              // never returns
}
