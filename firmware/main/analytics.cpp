#include "analytics.h"
#include <cmath>
#include <math.h>
#include "esp_log.h"

static const char* TAG = "ANALYTICS";

Analytics& Analytics::getInstance() {
    static Analytics instance;
    return instance;
}

float Analytics::calculateDewPoint(float temp, float hum) {
    if (hum <= 0.0f) hum = 0.001f;
    float a = 17.27f;
    float b = 237.7f;
    float alpha = ((a * temp) / (b + temp)) + logf(hum / 100.0f);
    return (b * alpha) / (a - alpha);
}

// Saturation vapour pressure, on the same Magnus constants as the dew point above — the two must
// agree, or the surface figure would contradict the dew point printed beside it in one message.
static float sat_vp(float t) { return 6.112f * expf(17.27f * t / (237.7f + t)); }

// DIN 4108-2 minimum for new construction, which is exactly the mould criterion. See the header.
static constexpr float F_RSI = 0.70f;

float Analytics::surfaceRH(float t_in, float rh_in, float t_out)
{
    if (!std::isfinite(t_out) || !std::isfinite(t_in) || !std::isfinite(rh_in)) return NAN;
    // A surface is never taken as warmer than the room: the sun does not shine on the cold corner
    // this is about, and allowing it would quietly turn a hot day into "no risk anywhere".
    const float t_surf = (t_out >= t_in) ? t_in : (t_out + F_RSI * (t_in - t_out));
    const float sat    = sat_vp(t_surf);
    if (!(sat > 0.0f)) return NAN;
    const float rh = 100.0f * sat_vp(t_in) * (rh_in / 100.0f) / sat;
    // Past saturation the surface is wet, not "120 % humid". The caller only compares against a
    // threshold below 100, and a capped number is the one worth showing the owner.
    return rh > 100.0f ? 100.0f : rh;
}

// BME690 datasheet BST-BME690-DS001-04, table 6. The index has a resolution of 1 (table 3), so the
// bands are taken on the rounded value: 150.4 is 150, «Lightly polluted».
IaqBand Analytics::iaqBand(float iaq) {
    const long v = lroundf(iaq);
    if (v <= 50)  return IAQ_EXCELLENT;
    if (v <= 100) return IAQ_GOOD;
    if (v <= 150) return IAQ_LIGHTLY;
    if (v <= 200) return IAQ_MODERATELY;
    if (v <= 250) return IAQ_HEAVILY;
    if (v <= 350) return IAQ_SEVERELY;
    return IAQ_EXTREMELY;
}

const char *Analytics::iaqBandName(IaqBand band) {
    static const char *const NAME[] = {
        "Excellent", "Good", "Lightly polluted", "Moderately polluted",
        "Heavily polluted", "Severely polluted", "Extremely polluted",
    };
    return NAME[band <= IAQ_EXTREMELY ? band : IAQ_EXTREMELY];
}

// The same table's "Suggested action" column, worded to follow a dash in a message: lower case, no full
// stop. Bosch's "even without people present" for 251-350 is left to the message, which says whether CO2
// points at people.
const char *Analytics::iaqBandAction(IaqBand band) {
    static const char *const ACTION[] = {
        "no measures needed",
        "no measures needed",
        "ventilation suggested",
        "increase ventilation with clean air",
        "optimize ventilation",
        "maximize ventilation, reduce time in the room and find the source",
        "maximize ventilation, avoid the room and find the source",
    };
    return ACTION[band <= IAQ_EXTREMELY ? band : IAQ_EXTREMELY];
}

// The same classes as the screen writes them (owner, 2026-09-14). The Air Quality block is 107 px wide
// and the glcd font at size 2 is 12 px a letter, so a word there has 8 letters at most and «Moderately
// polluted» cannot be one. "Pure" is Bosch's own word for 0-50, from the same row of table 6 ("Pure
// air"); the rest are the class names without "polluted". Telegram, MQTT and the console keep the full
// names.
const char *Analytics::iaqBandShort(IaqBand band) {
    static const char *const SHORT[] = { "Pure", "Good", "Light", "Moderate", "Heavy", "Severe", "Extreme" };
    return SHORT[band <= IAQ_EXTREMELY ? band : IAQ_EXTREMELY];
}

// Pressure tendency over three hours, named as the Met Office names it: a fall of 1.6-3.5 hPa is
// "falling", 3.6-6.0 "falling quickly", and more is faster still. Tendencies are reported in tenths of a
// hectopascal, so the change is rounded to 0.1 hPa before it is named — 3.5 is still "falling". The
// device shows "falling" as Rain and anything quicker as Storm.
int8_t Analytics::baroMode(float dp_hpa, float p_sea_hpa, int8_t prev) {
    // The level first: it decides what the fall MEANS (see analytics.h for the day this was
    // measured). An unknown level is not a reason to guess — a word we cannot stand behind is
    // worse than the plain number, so the caller gets "number" and the console says why.
    if (!std::isfinite(p_sea_hpa)) return 0;
    const float level_gate = (prev >= 1) ? BARO_LEVEL_CLEAR : BARO_LEVEL_TRIP;
    if (p_sea_hpa > level_gate) return 0;

    const float d = roundf(dp_hpa * 10.0f) / 10.0f;
    // Entering is unchanged — the boundaries are the Met Office's and nothing here may move them.
    // Only leaving is held back, and only for a mode that is already on the glass.
    if (d <= BARO_STORM_TRIP) return 2;
    if (prev >= 2 && d <= BARO_STORM_CLEAR) return 2;
    if (d <= BARO_RAIN_TRIP) return 1;
    if (prev >= 1 && d <= BARO_RAIN_CLEAR) return 1;
    return 0;
}

void Analytics::classifyAir(uint16_t co2_real, float iaq, float temp, float hum, bool hum_fresh,
                            uint8_t accuracy, float t_out, const AlertConfig &cfg, AlertState &st,
                            int &pollutionClass, std::string &pollutionText, int &airClass) {
    // AN011, IAQ accuracy: 0 is "stabilization / run-in ongoing"; from 1 up the index is output with
    // low, medium or high accuracy.
    const bool gas_ready = (accuracy >= 1);
    const IaqBand band = iaqBand(iaq);

    // --- Humidity: sustained + hysteresis (avoid shower spikes and flapping) ---
    //
    // The counter counts MEASUREMENTS, not calls, and that distinction was a defect until
    // 2026-10-02. This function runs on the gas beat — every 300 s — but the humidity comes from
    // the SCD41, which is read on the owner's interval: 15 min in the default profile, 30 in Ultra
    // Save. On the ticks in between, measure() seeds `hum` from rtc_hum, so the SAME reading was
    // handed over two or three times running and counted each time. One excursion over the trip
    // therefore tripped damp five minutes later, all by itself — and because alertPending() cites
    // this very "sustained over two cycles" as its reason to let damp skip the alert debounce, that
    // single reading went straight out as a WARNING: Telegram, and the radio raised at night too.
    // Exactly the shower spike the counter exists to absorb. Only Performance, where CO2 is read on
    // the same 5 min as the gas, ever behaved as written.
    //
    // WHAT is judged changed on 2026-10-02 (owner): the humidity at the coldest surface, not the
    // humidity of the room. Room air grows nothing; walls do, and a wall's humidity depends on how
    // cold it is, which is the season. The old fixed 70 % was wrong at both ends of the year and in
    // opposite directions — silent in January at 68 % with 98 % already on the wall, shouting in
    // July at 70 % with 77 % and no risk at all. The dew-point branch that went with it (over 65 %
    // with dew point over 18 C) only ever engaged above 23.8 C in the room, i.e. in summer, which is
    // precisely backwards for mould; it is gone. Muggy air is a comfort question and wants its own
    // answer, not this one's name.
    //
    // Without a weather reading there is no season to reason about, and the room number is all
    // there is — Matter, no_wifi, or simply offline. Said plainly rather than guessed around.
    const float srh      = surfaceRH(temp, hum, t_out);
    const bool  seasonal = std::isfinite(srh);
    const float level    = seasonal ? srh : hum;
    const float trip     = seasonal ? DAMP_SURF_TRIP  : DAMP_ROOM_TRIP;
    const float clear    = seasonal ? DAMP_SURF_CLEAR : DAMP_ROOM_CLEAR;
    const bool  hum_over = level > trip;
    if (hum_fresh) {
        if (hum_over) { if (st.hum_sustain < 255) st.hum_sustain++; }
        else          { st.hum_sustain = 0; }
    }
    // A stale tick leaves the counter alone and the latch below re-reads it unchanged: nothing was
    // learned, so nothing moves in either direction. The latch cannot be cleared by a stale value
    // either — the reading that set it was above the clear threshold by definition.
    // Trips after 2 consecutive over-cycles; once damp, stays until below CLEAR. The latch is its
    // own flag rather than "was the last reported class 5", so a tick of some other class cannot
    // wipe it — see the note on AlertState.
    bool damp = (st.hum_sustain >= 2) || (st.damp_latched && level > clear);
    st.damp_latched = damp;

    // --- CO2: hysteresis around the user's level (sticky once tripped) ---
    uint16_t co2_trip  = cfg.co2_level;
    uint16_t co2_clear = (cfg.co2_level > 150) ? (uint16_t)(cfg.co2_level - 150) : 0;
    bool co2_high = st.co2_latched ? (co2_real > co2_clear) : (co2_real > co2_trip);
    st.co2_latched = co2_high;

    // One window for the AIR, most urgent first; the first match wins. The gas classes are Bosch's
    // bands as they stand — no threshold of ours on the index. A broadband MOX sensor cannot name
    // the gas, so these are levels, never a substance. Moisture is deliberately absent here: it is
    // not a property of the air in the sense these classes mean, and the dashboard shows it beside
    // the humidity reading instead.
    if (gas_ready && band >= IAQ_HEAVILY) {
        airClass = 3; pollutionText = iaqBandName(band);
    }
    // 1 = Stuffy: real CO2 from the SCD41 is high (always trusted).
    else if (co2_high) {
        airClass = 1; pollutionText = "Stuffy";
    }
    // 2 = Moderately polluted. CO2 is below the owner's level here — class 1 would have won.
    else if (gas_ready && band == IAQ_MODERATELY) {
        airClass = 2; pollutionText = iaqBandName(band);
    }
    // 0 = no warning: Bosch's own word for the index.
    else {
        airClass = 0;
        pollutionText = gas_ready ? iaqBandName(band) : "Warming up";
    }

    // The overall verdict the owner is told about. Damp keeps EXACTLY the precedence it always had
    // — below heavy pollution, above stuffy — so the alert state machine, Telegram, MQTT and
    // /api/status see no change at all from the dashboard moving it elsewhere.
    pollutionClass = (airClass == 3) ? 3 : (damp ? 5 : airClass);
    if (pollutionClass == 5) pollutionText = "Damp";

    st.last_class = pollutionClass;
}

AlertTier Analytics::alertTier(int pollutionClass, uint16_t co2_real) {
    switch (pollutionClass) {
        // Class 3 was CRITICAL back when we believed it meant smoke or a gas leak. It does
        // not (see categoryEnabled). CRITICAL is the life-safety tier: it punches through
        // night mode and repeats every 15 min. Polluted air does not earn that — and it would
        // be incoherent anyway, since the user can switch class 3 off.
        // WARNING still wakes the radio and still reaches the user; it just does not shout
        // at 3 a.m. every quarter hour about paint fumes.
        case 3: return TIER_WARNING;                                   // IAQ > 200
        case 5: return TIER_WARNING;                                   // damp / mould
        case 1: return (co2_real >= 1500) ? TIER_WARNING : TIER_ADVISORY; // CO2, escalates
        case 2: return TIER_ADVISORY;                                  // IAQ 151..200
        default: return TIER_CLEAR;
    }
}

bool Analytics::categoryEnabled(int pollutionClass, const AlertConfig &cfg) {
    switch (pollutionClass) {
        // Class 3 used to be forced on with a "safety" justification, on the assumption it
        // detected smoke or a gas leak. Measured on hardware, it does not: a candle snuffed
        // directly over the sensor reached only IAQ 85 (Bosch's "Good" band) versus IAQ 500
        // for alcohol. Smoke is mostly particulates, which a MOX sensor cannot see at all,
        // and our 300 s cadence could not catch a transient anyway. What class 3 reliably
        // catches is a strong SOLVENT/VOC event — useful, but that is "open a window", not
        // life safety. So it belongs under the same toggle as class 2: an alert a user
        // cannot switch off implies a guarantee we cannot honour.
        case 3: return cfg.voc_en;
        case 1: return cfg.co2_en;
        case 2: return cfg.voc_en;
        case 5: return cfg.hum_en;
        default: return false;
    }
}

bool Analytics::alertPending(int pollutionClass, const AlertConfig &cfg, AlertState &st,
                             int &confirmed) {
    // A category the owner switched off is not an event. Folding it to 0 here rather than at the
    // sending end means switching a category off also produces the one all-clear that closes
    // whatever was last said about it, instead of leaving the last word hanging.
    const int effective = (pollutionClass != 0 && categoryEnabled(pollutionClass, cfg))
                            ? pollutionClass : 0;

    if (effective != st.cand_class) { st.cand_class = effective; st.cand_count = 1; }
    else if (st.cand_count < 250)   { st.cand_count++; }

    // Two ticks are demanded only where the input actually wobbles. That protection was written
    // for the GAS signal, which can hand back a bad first sample after the algorithm re-initialises
    // — and gas moves on its own five-minute beat, so waiting costs nothing anyone notices.
    //
    // CO2 and damp are different and were being punished for the gas sensor's problem. Both come
    // from the SCD41, both are absolute readings, and both already carry protection of their own:
    // CO2 has hysteresis (trips at the level, clears 150 lower, sticky in between) and damp needs
    // humidity over the trip on two consecutive REAL measurements before it trips at all. Making
    // them wait for a second tick as well meant pressing the button at 1500 ppm produced silence,
    // because the confirming tick was a gas tick up to five minutes away — by which time the
    // reading had fallen back.
    //
    // That word REAL is load-bearing and was not true until 2026-10-02: the damp counter advanced
    // on re-used readings, so the protection this paragraph leans on did not exist in the default
    // profile. If the counter ever stops counting measurements, this exemption goes with it.
    //
    // Clearing is debounced whenever either side of the transition is a gas class, so an all-clear
    // for a gas event still has to be seen twice. Class 4 is counted as gas so that a device updated
    // in the middle of an old «stale air» event closes it the same way.
    auto is_gas = [](int c) { return c == 2 || c == 3 || c == 4; };
    const bool needs_two = is_gas(effective) || is_gas(st.notified_class);
    confirmed = (!needs_two || st.cand_count >= 2) ? st.cand_class : st.notified_class;
    if (confirmed == st.notified_class) return false;

    ESP_LOGW(TAG, ">>> AIR %s: class %d -> %d <<<",
             confirmed == 0 ? "CLEARED" : "ALERT", st.notified_class, confirmed);
    return true;
}

void Analytics::alertSent(AlertState &st, int confirmed, time_t now) {
    st.notified_class = confirmed;
    st.last_notify    = now;
}
