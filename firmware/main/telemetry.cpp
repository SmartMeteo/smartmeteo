// ============================================================================
// Shared telemetry — the one place the readings live between the measuring cycle and the web
// console. The cycle (bench_screens.cpp) writes them once per wake; /api/status reads them.
//
// They sit in their own file because they outlived the code they used to share a file with. The
// previous production cycle defined them alongside three thousand lines of its own logic, so the
// dead cycle could not be removed without taking the live console's data with it. Split out, the
// old file could go; see legacy/ for what was kept for reference.
//
// Anything added here must be published by the cycle (`publish_globals`) — a variable that only
// the web server reads is a field that will silently report zero.
// ============================================================================
#include <cmath>
#include <stdint.h>
#include <string>

// --- battery -------------------------------------------------------------------------------
float    global_voltage = 0.0f;
float    global_soc = 0.0f;
// The cell as measured, before battery_display.h shaped it for the glass. Time to empty is a
// question about the cell, so the battery card and the hub divide THIS by the measured rate.
float    global_soc_raw = 0.0f;
float    global_crate = 0.0f;            // %/h, signed; charging is positive
int      global_bat_state = 0;           // -1 discharging, 0 idle, 1 charging
float    global_bat_ttf_h = -1.0f;       // time to full, hours
bool     global_bat_low = false;
uint8_t  global_bat_alerts = 0;          // MAX17048 latched STATUS flags
uint32_t global_battery_cycles = 0;
bool     global_on_usb = false;

// --- climate and air -----------------------------------------------------------------------
uint16_t global_co2 = 0;
float    global_s_temp = 0.0f;           // SCD41 — the room climate that gets displayed
float    global_s_hum = 0.0f;
float    global_b_temp = 0.0f;           // BME690 — biased by its own gas heater, diagnostics only
float    global_b_hum = 0.0f;
float    global_b_pres = 0.0f;           // Pa; 0 = no usable reading (see CLAUDE.md 4.17)
float    global_b_gas = 0.0f;            // raw gas resistance, Ohm
float    global_iaq = 0.0f;
float    global_static_iaq = 0.0f;
float    global_gas_percentage = 0.0f;
uint8_t  global_iaq_accuracy = 0;
float    global_stab_status = 0.0f;
float    global_run_in_status = 0.0f;
float    global_dewpoint = 0.0f;
int      global_pollution_class = 0;
std::string global_pollution_text = "";

// --- outdoor weather -----------------------------------------------------------------------
float    global_out_temp = -999.0f;      // -999 = nothing fetched yet
std::string global_out_desc = "";
std::string global_out_city = "";
std::string global_out_icon = "";
// "off" (not configured) / "ok" / "err" (last fetch failed — bad key, bad city, or no network)
std::string global_wx_state = "off";
// The four extra readings the bottom strip can show. Published so the owner can SEE which of them
// the hub actually sent: a weather integration without uv_index leaves the key out, the device
// keeps its sentinel, and the cell shows dashes — indistinguishable on the glass from a field that
// is broken. Same sentinels as in the cycle's RTC copy, because -1 %% cloud is not a reading and 0 %% is.
// (Rain chance was here until 2026-09-28; MET publishes it only inside the Nordics, so it was
// dropped from the strip and is no longer fetched at all.)
float    global_out_uv    = -1.0f;       // -1 = the hub never sent it
float    global_out_dew   = -999.0f;
// Humidity at the coldest indoor surface — what the damp verdict is actually judged on since
// 2026-10-02. NAN while there is no current outdoor reading to work it out from, which is itself
// the answer to "why is it not saying anything about damp".
float    global_surface_rh = NAN;
// The barometric plaque's second input, published so the console can explain itself. An owner shown
// a bare number instead of a forecast must be able to see WHY: never saw weather, or never got a
// city. Silence with no reason reads as a broken device.
float    global_p_sea    = NAN;   // local reading reduced to sea level, hPa
float    global_baro_k   = NAN;   // the altitude factor in force
uint8_t  global_baro_src = 0;     // 0 none, 1 measured against a weather report, 2 city altitude
int      global_out_cloud = -1;

// --- system --------------------------------------------------------------------------------
uint32_t global_boot_count = 0;

// --- portal / console handshakes -------------------------------------------------------------
int      global_portal_remain = -1;      // seconds left in the setup window; -1 = not open
uint32_t global_last_http_ms = 0;        // stamped by DELIBERATE requests only, so the window's
                                         // countdown is not restarted by the page's own polling
bool     global_portal_keep = false;     // user tapped the timer pill to stretch the window

// The web handler cannot touch the CO2 sensor — the cycle owns it — so a forced recalibration is
// requested here and performed by the cycle, which reports back through result/correction.
volatile int global_calib_req = 0;
volatile int global_calib_result = 0;    // 0 idle, 2 running, 3 asking, 1 done, -1 failed
volatile int global_calib_correction = 0;
volatile int global_calib_samples = 0;   // run-up readings taken since the request
volatile int global_calib_error = 0;     // 1 window closed, 2 sensor refused, 3 no idle sensor,
                                         // 4 the air was not outdoor air and nobody said to go on
// What the run-up actually breathed. Sensirion prescribes the run-up and the 420 ppm reference but
// gives no test for the air itself, and the one mistake that matters here cannot be undone: press
// this indoors and every future reading is wrong. So the device reports what it read and, when that
// is not outdoor air, asks before anchoring anything.
volatile int global_calib_lo = 0;        // lowest run-up reading, ppm
volatile int global_calib_hi = 0;        // highest
volatile int global_calib_avg = 0;       // mean of the run-up readings
volatile int global_calib_confirm = 0;   // the page's answer while result == 3: 1 go on, -1 stop

// Same arrangement for joining a network from inside the setup wizard: the save handler asks, the
// portal loop connects while keeping its own access point up so the phone does not lose the page.
// State: 0 idle, 1 connecting, 2 connected, -1 failed.
volatile int global_wifi_connect_req = 0;
volatile int global_wifi_connect_state = 0;
// Set when the city or API key changes, so a freshly configured device does not sit out the
// half-hour fetch throttle before it first shows the weather.
volatile bool global_weather_dirty = false;
// The broker or the topic changed: what Home Assistant was told about this device no longer
// describes where it publishes. See rtc_mqtt_disco_at in bench_screens.cpp.
volatile bool global_hub_dirty = false;
// The run-up started before the readings had stopped falling (the 20-reading cap). Said in the
// result and in /api/status, never a refusal: a calibration made in doubtful air is undone by the
// next one, and the owner is the only one who can judge the air.
volatile int global_calib_unsettled = 0;
// Which half of the run is under way: the console must not say "reading 0 of 6" while the device
// is still waiting for the air in its case to change.
volatile int global_calib_settled = 0;
// Settings that change what the GLASS shows were just saved: the cycle repaints at once instead of
// waiting for the next wake, because the user is standing in front of the device having pressed
// Apply and expects to see the result.
volatile bool global_screen_dirty = false;
// The page is going away (tab closed, or the user asked to close setup): shut the window now
// rather than leaving the radio up for the rest of its three minutes.
volatile bool global_portal_close = false;
// Factory reset asked for by the console. The handler only raises this. The erase and the restart
// belong to the cycle: it owns the panel, the sensors and the radio, and it is the only task that
// can make sure nothing writes a setting back between the request and the restart.
volatile bool global_factory_req = false;
// A restart asked for by the console. Same reasoning: the handler answers and raises this, and the
// window loop restarts from the task that owns the panel, once any frame it is drawing has finished.
volatile bool global_reboot_req = false;
volatile bool global_close_req = false;   // console asked to end the session now
volatile bool global_poweroff_req = false;
// Room-temperature readings taken with the SCD41 offset now on file (bench_screens.cpp,
// scd_offset_settled). The temperature calibration measures against one of these at most once.
volatile uint32_t global_temp_seq = 0;

// Falling edges on the button line that were not presses. Published so a battery run can be
// watched from the hub instead of from a serial port that does not exist while it runs.
uint32_t global_btn_noise = 0;
