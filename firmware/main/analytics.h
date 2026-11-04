#pragma once

#include <stdint.h>
#include <time.h>
#include <string>

// Severity tiers drive the notification/wake policy (see analytics.cpp):
//   CRITICAL punches through the schedule (even at night), repeats often;
//   WARNING  punches through, ~1 h cooldown;
//   ADVISORY waits for the next scheduled radio window, ~3 h cooldown.
enum AlertTier : uint8_t { TIER_CLEAR = 0, TIER_ADVISORY = 1, TIER_WARNING = 2, TIER_CRITICAL = 3 };

// User-tunable alert settings (from NVS). There is no IAQ threshold to tune: the gas classes are
// Bosch's own classification (IaqBand). Only the CO2 level and per-category switches.
struct AlertConfig {
    uint16_t co2_level = 1100;   // class-1 CO2 trip point (NVS "al_co2"; band in co2_alert.h)
    bool     co2_en    = true;   // NVS "al_co2en" — CO2 (stuffy) alerts
    bool     voc_en    = true;   // NVS "al_voc"   — polluted-air alerts, classes 2 and 3 (gas sensor)
    bool     hum_en    = true;   // NVS "al_hum"   — damp/mould alerts
};

// Bosch's classification of the index: BME690 datasheet BST-BME690-DS001-04, table 6. It is given
// for BSEC_OUTPUT_IAQ, the 0..500 index BSEC keeps calibrated itself over the recent days so that
// IAQ 50 is typical good air and 200 typical polluted air (AN011, BSEC_OUTPUT_IAQ). Bosch gives no
// scale for the static IAQ, so nothing is classified on it.
enum IaqBand : uint8_t {
    IAQ_EXCELLENT = 0,   //   0-50   no measures needed
    IAQ_GOOD,            //  51-100  no measures needed
    IAQ_LIGHTLY,         // 101-150  ventilation suggested
    IAQ_MODERATELY,      // 151-200  increase ventilation with clean air
    IAQ_HEAVILY,         // 201-250  optimize ventilation
    IAQ_SEVERELY,        // 251-350  identify the contamination, maximize ventilation
    IAQ_EXTREMELY,       // 351-500  identify the contamination, avoid the room
};

// Alert lifecycle state persisted in RTC memory across deep sleep. Zero-init.
struct AlertState {
    int     last_class     = 0;  // classifier output last cycle (for hysteresis)
    uint8_t hum_sustain    = 0;  // consecutive cycles humidity has been over the damp trip
    // One latch per category, because the cascade only ever reports its WINNER. Both latches used
    // to be read back out of last_class, so a single tick of a higher class erased the memory of a
    // lower one that was still true: strong pollution (3) outranks damp (5) and stuffy (1), so one
    // gas event dropped a room sitting inside the CO2 hysteresis band straight to "Good" and sent
    // the owner an all-clear for a condition that had never cleared. Nothing about the air changed.
    bool    co2_latched    = false;  // CO2 tripped and has not fallen below the clear threshold
    bool    damp_latched   = false;  // humidity tripped and has not fallen below the clear threshold
    int     notified_class = 0;  // class the owner was last told about (0 = last word was all-clear)
    time_t  last_notify    = 0;  // when we last notified
    // Debounce. The classifier is rebuilt on every wake and its gas input can wobble on the first
    // sample after a re-init, so a class has to be seen twice running before it is believed.
    int     cand_class     = 0;  // class currently being considered
    uint8_t cand_count     = 0;  // how many consecutive ticks it has held
};

class Analytics {
public:
    static Analytics& getInstance();

    float calculateDewPoint(float temp, float hum);

    // Relative humidity AT THE COLDEST INDOOR SURFACE — the thing mould actually answers to, since
    // room air never grows anything. A wall is colder than the room by an amount the outdoor
    // temperature sets, so the same 70 % in the room is condensation in January and harmless in
    // July: at 21 C indoors the surface reaches the 80 % mould limit at 54 % room humidity when it
    // is 0 C outside, and only at 86 % when it is 25 C. Judging on the room number alone is wrong
    // at BOTH ends of the year, and it was wrong in opposite directions.
    //
    //     T_surface = T_out + f * (T_in - T_out)
    //
    // f is a property of the wall. DIN 4108-2 requires f >= 0.70 of new German construction, and
    // that requirement IS the mould criterion — so judging against 0.70 says "this much moisture
    // would grow mould in a building that meets code". Older cold corners run 0.50..0.60 and would
    // want warning sooner; the device cannot know which it stands in, and that limit is named here
    // rather than hidden. NAN when the outdoor temperature is unknown.
    static float surfaceRH(float t_in, float rh_in, float t_out);

    // EN ISO 13788 names 80 % at the surface as the limit to stay under, and ASHRAE 160 the same
    // over a 30-day mean. Below 70 % nothing grows at all, which is why the latch clears there — a
    // wide band on purpose, because mould is slow and a flapping warning about it is noise.
    static constexpr float DAMP_SURF_TRIP  = 80.0f;
    static constexpr float DAMP_SURF_CLEAR = 70.0f;
    // With no weather reading there is no season to reason about — Matter, no_wifi, or just
    // offline. Then the room number is all there is, on the thresholds it always used.
    static constexpr float DAMP_ROOM_TRIP  = 70.0f;
    static constexpr float DAMP_ROOM_CLEAR = 62.0f;

    static IaqBand     iaqBand(float iaq);
    static const char *iaqBandName(IaqBand band);     // Bosch's words: "Excellent" .. "Extremely polluted"
    static const char *iaqBandAction(IaqBand band);   // Bosch's suggested action for the band
    static const char *iaqBandShort(IaqBand band);    // the screen's form, 8 letters at most: "Pure" .. "Extreme"
    // Barometric forecast from the pressure change over three hours, in hPa: 0 none, 1 Rain, 2 Storm.
    // `prev` is what the plaque shows now, and it only ever makes the mode HOLD: entering still takes
    // the Met Office boundary, leaving takes 0.4 hPa more. Pass 0 to decide with no memory, which is
    // what a tick that cannot read the barometer or find a record three hours back must do.
    //
    // The margin is sized by the wobble that is actually there, and it is not sensor noise (the
    // BME690 resolves 0.013 hPa). It is the BASELINE HOP: "three hours ago" is the nearest record
    // within +-15 min, so as the window slides the comparison jumps to the neighbouring record and
    // the difference jumps with it by one step of the fall — fall/12 at a 15-minute history step,
    // which is 0.3 hPa for a fall sitting right at the Storm boundary. 0.4 covers that with room,
    // and costs about half an hour of extra Rain when a fall eases at the rate seen on 2026-10-05
    // (0.9 hPa per hour).
    static constexpr float BARO_RAIN_TRIP   = -1.6f;   // Met Office "falling"
    static constexpr float BARO_RAIN_CLEAR  = -1.2f;
    static constexpr float BARO_STORM_TRIP  = -3.6f;   // Met Office "falling quickly"
    static constexpr float BARO_STORM_CLEAR = -3.2f;

    // ...and a SECOND input, because the rate alone forecasts nothing. The same 1.6 hPa/3h is a
    // weakening high at 1023 hPa and an arriving low at 1008. Measured here on 2026-10-05: the
    // barometer fell 3.9 hPa over seven hours and the plaque said Rain over a sunny afternoon —
    // MET had clear sky and 0.0 mm for the next 44 hours, and the rain finally arrived two days
    // later, when the pressure had come down to ~1008. Every classical barometer reads both: the
    // dial prints RAIN / CHANGE / FAIR around pressure marks, not around rates.
    //
    // The boundary is the standard atmosphere, 1013.25 hPa — not a number we invented, and the
    // conventional line between above-normal and below-normal pressure. Leaving is held back by
    // 1 hPa for the same reason the rate is: the reduction factor is only good to a few tenths,
    // and a verdict that flickers at the boundary is worse than one that lingers.
    static constexpr float BARO_LEVEL_TRIP  = 1013.25f;  // ICAO standard atmosphere
    static constexpr float BARO_LEVEL_CLEAR = 1014.25f;
    // `p_sea_hpa` is the local reading reduced to sea level. NAN means the device has never
    // learned its altitude factor — then there is no forecast at all, only the number.
    static int8_t baroMode(float dp_hpa, float p_sea_hpa, int8_t prev = 0);

    // The one verdict for the air — the Air Quality block, Telegram, MQTT and the console all show
    // this word. Both sensors feed the same window, most urgent first; the first match wins:
    //   3  IAQ above 200: Bosch «Heavily», «Severely» or «Extremely polluted»   gas sensor
    //   5  damp: humidity sustained high                                         SCD41
    //   1  stuffy: CO2 at or above the owner's level                             SCD41
    //   2  IAQ 151..200: Bosch «Moderately polluted»                            gas sensor
    //   0  no warning: Bosch's word for the index, or «Warming up» at accuracy 0
    // Class 4 (stale air) is retired; records from 0.9.54 and older can still carry it.
    // The gas classes need accuracy >= 1 — Bosch's 0 is "stabilization / run-in ongoing" (AN011).
    // CO2 and humidity are absolute readings and are trusted from the first measurement, so they
    // stay visible while the gas sensor is still stabilising. `cfg` supplies the CO2 trip level;
    // `st` carries the CO2 and damp hysteresis across sleeps.
    //
    // `hum_fresh` says whether `temp`/`hum` were MEASURED on this call or carried over from the
    // last one, and the damp counter moves only when they were. The caller runs on the gas beat
    // (300 s) while the SCD41 is read on the owner's own interval (5/15/30 min by profile), so on
    // the ticks in between it hands over the previous reading out of RTC — and without this flag
    // the counter advanced on a value nothing had re-measured. See classifyAir for what that cost.
    //
    // `airClass` is the SAME cascade with moisture taken out — the verdict about the air itself.
    // The dashboard needs both, because damp belongs beside the humidity it was read from, not on
    // top of the air verdict: until 2026-10-02 a damp room hid the gas word behind «Damp», so a
    // room that was damp AND polluted reported only the moisture. `pollutionClass` is unchanged and
    // still carries damp — it is what the owner is told through Telegram, MQTT and /api/status.
    //
    // `t_out` is the outdoor temperature for the damp judgement, or NAN when none is current — see
    // surfaceRH(). With it the verdict follows the season; without it, the room number alone.
    void classifyAir(uint16_t co2_real, float iaq, float temp, float hum, bool hum_fresh,
                     uint8_t accuracy, float t_out, const AlertConfig &cfg, AlertState &st,
                     int &pollutionClass, std::string &pollutionText, int &airClass);

    // Severity tier for a class (CO2 escalates to WARNING when very high).
    static AlertTier alertTier(int pollutionClass, uint16_t co2_real);
    // Whether the owner has this class's category switched on.
    static bool categoryEnabled(int pollutionClass, const AlertConfig &cfg);

    // Is there anything NEW to tell the owner? Pure and cheap: no clock, no radio, no side effect
    // beyond the debounce counters, so it runs on the ordinary measurement tick. `confirmed` comes
    // back as the class the device is willing to stand behind — the current one once it has held
    // still, otherwise the last one already reported.
    //
    // Two rules, and they exist for the same reason: the radio is the most expensive thing this
    // device does, so it must only be raised for something worth saying.
    //
    // EDGE, NOT STATE. A message goes out when the situation CHANGES — once when the air goes bad,
    // once when it comes back. Not while it is bad. The previous design re-sent on a timer, which
    // meant the same fact arrived hourly, and any window that happened to open — somebody pressing
    // the button to look at the console — carried a copy of it.
    //
    // AND IT HAS TO HOLD STILL FIRST. The classifier is recomputed from scratch on every wake, and
    // the gas algorithm's first sample after each re-init can come out low, so the class can flick
    // between two values on consecutive ticks. Edge-triggering on a flickering signal is a message
    // per flick — worse than the timer it replaced. So a new class must be seen twice in a row
    // before it counts, and a reading that cannot make up its mind is not news.
    bool alertPending(int pollutionClass, const AlertConfig &cfg, AlertState &st, int &confirmed);

    // Call once the message for `confirmed` has actually been handed to delivery. Kept separate so
    // that a window the device failed to open cannot silently consume the event.
    void alertSent(AlertState &st, int confirmed, time_t now);

private:
    Analytics() = default;
    ~Analytics() = default;
};
